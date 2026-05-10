#include "RenderQueueDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {

QString statusToDisplay(RenderJobStatus s)
{
    switch (s) {
    case RenderJobStatus::Pending:   return QStringLiteral("待機");
    case RenderJobStatus::Rendering: return QStringLiteral("実行中");
    case RenderJobStatus::Completed: return QStringLiteral("完了");
    case RenderJobStatus::Failed:    return QStringLiteral("失敗");
    case RenderJobStatus::Cancelled: return QStringLiteral("中止");
    }
    return QStringLiteral("待機");
}

QString humanRange(qint64 startUs, qint64 endUs)
{
    if (startUs == 0 && endUs == 0)
        return QStringLiteral("全体");
    auto fmt = [](qint64 us) {
        const qint64 totalMs = us / 1000;
        const int h = static_cast<int>((totalMs / 3600000) % 100);
        const int m = static_cast<int>((totalMs / 60000) % 60);
        const int s = static_cast<int>((totalMs / 1000) % 60);
        return QString::asprintf("%02d:%02d:%02d", h, m, s);
    };
    if (endUs <= 0)
        return fmt(startUs) + QStringLiteral("〜");
    return fmt(startUs) + QStringLiteral("〜") + fmt(endUs);
}

} // namespace


// ---------------------------------------------------------------------------
// AddRenderJobDialog
// ---------------------------------------------------------------------------
// Modal "configure new job" sub-dialog. Output picker, preset combo, range
// spinboxes (in seconds for human friendliness). On accept the caller reads
// `job()` and hands it to RenderQueue::addJob().

class AddRenderJobDialog : public QDialog
{
public:
    AddRenderJobDialog(qint64 defaultStartUs, qint64 defaultEndUs, QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("ジョブ追加"));
        setModal(true);

        auto *form = new QFormLayout;

        // Source / project file (optional — if blank, RenderQueue uses the
        // currently-open project path implicitly when MainWindow wires it).
        m_sourceEdit = new QLineEdit(this);
        m_sourceEdit->setPlaceholderText(QStringLiteral("(現在開いているプロジェクト)"));
        auto *sourceBrowse = new QPushButton(QStringLiteral("..."), this);
        sourceBrowse->setMaximumWidth(40);
        connect(sourceBrowse, &QPushButton::clicked, this, [this]() {
            const QString p = QFileDialog::getOpenFileName(
                this, QStringLiteral("入力ファイル"), QString(),
                QStringLiteral("Video files (*.mp4 *.mov *.mkv *.webm);;All files (*.*)"));
            if (!p.isEmpty())
                m_sourceEdit->setText(p);
        });
        auto *sourceRow = new QHBoxLayout;
        sourceRow->addWidget(m_sourceEdit, 1);
        sourceRow->addWidget(sourceBrowse);
        form->addRow(QStringLiteral("入力:"), sourceRow);

        // Output path picker.
        m_outputEdit = new QLineEdit(this);
        m_outputEdit->setPlaceholderText(QStringLiteral("output.mp4"));
        auto *outputBrowse = new QPushButton(QStringLiteral("..."), this);
        outputBrowse->setMaximumWidth(40);
        connect(outputBrowse, &QPushButton::clicked, this, [this]() {
            const QString p = QFileDialog::getSaveFileName(
                this, QStringLiteral("出力先"), QString(),
                QStringLiteral("MP4 (*.mp4);;MOV (*.mov);;MKV (*.mkv);;WebM (*.webm);;All files (*.*)"));
            if (!p.isEmpty())
                m_outputEdit->setText(p);
        });
        auto *outputRow = new QHBoxLayout;
        outputRow->addWidget(m_outputEdit, 1);
        outputRow->addWidget(outputBrowse);
        form->addRow(QStringLiteral("出力:"), outputRow);

        // Preset selector.
        m_presetCombo = new QComboBox(this);
        for (const RenderPreset &p : RenderQueue::availablePresets())
            m_presetCombo->addItem(p.name);
        form->addRow(QStringLiteral("プリセット:"), m_presetCombo);

        // Range — defaults seeded by the queue dialog's setDefaultTimelineRange.
        m_startSpin = new QSpinBox(this);
        m_startSpin->setRange(0, 24 * 3600 * 1000);   // 24h in ms — generous
        m_startSpin->setSuffix(QStringLiteral(" ms"));
        m_startSpin->setValue(static_cast<int>(defaultStartUs / 1000));

        m_endSpin = new QSpinBox(this);
        m_endSpin->setRange(0, 24 * 3600 * 1000);
        m_endSpin->setSuffix(QStringLiteral(" ms"));
        m_endSpin->setSpecialValueText(QStringLiteral("(末尾まで)"));
        m_endSpin->setValue(static_cast<int>(defaultEndUs / 1000));

        // 2-pass VBR toggle.
        m_passesCombo = new QComboBox(this);
        m_passesCombo->addItem(QStringLiteral("1-pass"));
        m_passesCombo->addItem(QStringLiteral("2-pass (VBR)"));

        auto *rangeRow = new QHBoxLayout;
        rangeRow->addWidget(m_startSpin);
        rangeRow->addWidget(new QLabel(QStringLiteral("〜")));
        rangeRow->addWidget(m_endSpin);
        form->addRow(QStringLiteral("範囲:"), rangeRow);
        form->addRow(QStringLiteral("パス:"), m_passesCombo);

        auto *root = new QVBoxLayout(this);
        root->addLayout(form);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(buttons);

        resize(520, 0);
    }

    // Returns the configured job. Caller is responsible for assigning a
    // sensible projectFilePath if m_sourceEdit was left blank.
    RenderJob job() const
    {
        const auto presets = RenderQueue::availablePresets();
        const int idx = qBound(0, m_presetCombo->currentIndex(), presets.size() - 1);
        const RenderPreset &p = presets[idx];

        const qint64 startUs = static_cast<qint64>(m_startSpin->value()) * 1000;
        const qint64 endUs = static_cast<qint64>(m_endSpin->value()) * 1000;

        RenderJob j = RenderQueue::jobFromPreset(p, m_outputEdit->text().trimmed(),
                                                 startUs, endUs);
        j.projectFilePath = m_sourceEdit->text().trimmed();
        j.passes = (m_passesCombo->currentIndex() == 1) ? 2 : 1;
        if (j.name.isEmpty())
            j.name = p.name;
        return j;
    }

private:
    QLineEdit *m_sourceEdit  = nullptr;
    QLineEdit *m_outputEdit  = nullptr;
    QComboBox *m_presetCombo = nullptr;
    QSpinBox  *m_startSpin   = nullptr;
    QSpinBox  *m_endSpin     = nullptr;
    QComboBox *m_passesCombo = nullptr;
};


// ---------------------------------------------------------------------------
// RenderQueueDialog
// ---------------------------------------------------------------------------

RenderQueueDialog::RenderQueueDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("レンダーキュー"));
    setModal(false);

    m_queue = new RenderQueue(this);
    m_ownsQueue = true;

    buildUi();

    connect(m_queue, &RenderQueue::jobsChanged,
            this, &RenderQueueDialog::onJobsChanged);
    connect(m_queue, &RenderQueue::jobProgressUuid,
            this, &RenderQueueDialog::onJobProgress);
    connect(m_queue, &RenderQueue::jobCompletedUuid,
            this, &RenderQueueDialog::onJobCompleted);
    connect(m_queue, &RenderQueue::allCompleted,
            this, &RenderQueueDialog::onAllCompleted);

    rebuildTable();
    updateButtons();
}

RenderQueueDialog::~RenderQueueDialog() = default;

void RenderQueueDialog::setQueue(RenderQueue *queue)
{
    if (queue == m_queue || !queue)
        return;

    if (m_queue) {
        disconnect(m_queue, nullptr, this, nullptr);
        if (m_ownsQueue)
            m_queue->deleteLater();
    }

    m_queue = queue;
    m_ownsQueue = false;

    connect(m_queue, &RenderQueue::jobsChanged,
            this, &RenderQueueDialog::onJobsChanged);
    connect(m_queue, &RenderQueue::jobProgressUuid,
            this, &RenderQueueDialog::onJobProgress);
    connect(m_queue, &RenderQueue::jobCompletedUuid,
            this, &RenderQueueDialog::onJobCompleted);
    connect(m_queue, &RenderQueue::allCompleted,
            this, &RenderQueueDialog::onAllCompleted);

    rebuildTable();
    updateButtons();
}

void RenderQueueDialog::setDefaultTimelineRange(qint64 startUs, qint64 endUs)
{
    m_defaultStartUs = startUs;
    m_defaultEndUs   = endUs;
}

void RenderQueueDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);

    // Top: action toolbar.
    auto *toolbar = new QHBoxLayout;
    m_addBtn    = new QPushButton(QStringLiteral("ジョブ追加"), this);
    m_removeBtn = new QPushButton(QStringLiteral("削除"), this);
    m_startBtn  = new QPushButton(QStringLiteral("開始"), this);
    m_stopBtn   = new QPushButton(QStringLiteral("停止"), this);
    toolbar->addWidget(m_addBtn);
    toolbar->addWidget(m_removeBtn);
    toolbar->addStretch(1);
    toolbar->addWidget(m_startBtn);
    toolbar->addWidget(m_stopBtn);
    root->addLayout(toolbar);

    // Table — id is hidden (uuid stored in row's first item user data).
    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(
        QStringList{ QStringLiteral("ID"),
                     QStringLiteral("プリセット"),
                     QStringLiteral("範囲"),
                     QStringLiteral("状態"),
                     QStringLiteral("進捗") });
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(0,  50);
    m_table->setColumnWidth(1, 220);
    m_table->setColumnWidth(2, 130);
    m_table->setColumnWidth(3,  80);
    root->addWidget(m_table, 1);

    // Bottom: status label.
    m_statusLabel = new QLabel(QStringLiteral("キューは空です"), this);
    root->addWidget(m_statusLabel);

    // Close button.
    m_closeBtn = new QPushButton(QStringLiteral("閉じる"), this);
    auto *closeRow = new QHBoxLayout;
    closeRow->addStretch(1);
    closeRow->addWidget(m_closeBtn);
    root->addLayout(closeRow);

    connect(m_addBtn,    &QPushButton::clicked, this, &RenderQueueDialog::onAddJobClicked);
    connect(m_removeBtn, &QPushButton::clicked, this, &RenderQueueDialog::onRemoveJobClicked);
    connect(m_startBtn,  &QPushButton::clicked, this, &RenderQueueDialog::onStartClicked);
    connect(m_stopBtn,   &QPushButton::clicked, this, &RenderQueueDialog::onStopClicked);
    connect(m_closeBtn,  &QPushButton::clicked, this, &QDialog::close);
    connect(m_table,     &QTableWidget::itemSelectionChanged,
            this, &RenderQueueDialog::updateButtons);

    resize(720, 360);
}

void RenderQueueDialog::rebuildTable()
{
    if (!m_queue || !m_table)
        return;

    const QVector<RenderJob> jobs = m_queue->jobs();
    m_table->setRowCount(jobs.size());

    for (int row = 0; row < jobs.size(); ++row) {
        const RenderJob &j = jobs[row];

        auto *idItem = new QTableWidgetItem(QString::number(j.id));
        idItem->setData(Qt::UserRole, j.uuid);
        m_table->setItem(row, 0, idItem);

        const QString presetText = j.preset.isEmpty() ? j.name : j.preset;
        m_table->setItem(row, 1, new QTableWidgetItem(presetText));

        m_table->setItem(row, 2, new QTableWidgetItem(humanRange(j.startUs, j.endUs)));

        m_table->setItem(row, 3, new QTableWidgetItem(statusToDisplay(j.status)));

        // Progress bar lives in column 4 — insert / refresh.
        auto *bar = qobject_cast<QProgressBar *>(m_table->cellWidget(row, 4));
        if (!bar) {
            bar = new QProgressBar(m_table);
            bar->setRange(0, 100);
            bar->setTextVisible(true);
            m_table->setCellWidget(row, 4, bar);
        }
        bar->setValue(qBound(0, j.progressPercent, 100));
    }

    // Status label.
    if (jobs.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("キューは空です"));
    } else {
        const int pending   = m_queue->pendingCount();
        const int completed = m_queue->completedCount();
        const QString state = m_queue->isRunning()
            ? QStringLiteral("実行中")
            : QStringLiteral("待機中");
        m_statusLabel->setText(QString(QStringLiteral("%1 / %2 件完了 — %3"))
                                   .arg(completed).arg(jobs.size()).arg(state));
    }

    updateButtons();
}

void RenderQueueDialog::updateButtons()
{
    if (!m_queue || !m_addBtn)
        return;

    const bool running   = m_queue->isRunning();
    const bool hasPending= m_queue->pendingCount() > 0;
    const bool hasSelect = !m_table->selectedItems().isEmpty();

    m_addBtn   ->setEnabled(true);
    m_startBtn ->setEnabled(!running && hasPending);
    m_stopBtn  ->setEnabled(running);
    m_removeBtn->setEnabled(hasSelect && !running);
}

int RenderQueueDialog::rowForUuid(const QString &uuid) const
{
    if (!m_table)
        return -1;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *item = m_table->item(row, 0);
        if (item && item->data(Qt::UserRole).toString() == uuid)
            return row;
    }
    return -1;
}

QString RenderQueueDialog::uuidForRow(int row) const
{
    if (!m_table || row < 0 || row >= m_table->rowCount())
        return QString();
    QTableWidgetItem *item = m_table->item(row, 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void RenderQueueDialog::onAddJobClicked()
{
    AddRenderJobDialog dlg(m_defaultStartUs, m_defaultEndUs, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    RenderJob j = dlg.job();
    if (j.outputPath.trimmed().isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("出力先未指定"),
                             QStringLiteral("出力ファイルを指定してください。"));
        return;
    }

    m_queue->addJob(j);
}

void RenderQueueDialog::onRemoveJobClicked()
{
    const int row = m_table->currentRow();
    const QString uuid = uuidForRow(row);
    if (uuid.isEmpty())
        return;
    m_queue->removeJob(uuid);
}

void RenderQueueDialog::onStartClicked()
{
    if (!m_queue)
        return;
    m_queue->start();
    updateButtons();
}

void RenderQueueDialog::onStopClicked()
{
    if (!m_queue)
        return;
    m_queue->stop();
    updateButtons();
}

void RenderQueueDialog::onJobsChanged()
{
    rebuildTable();
}

void RenderQueueDialog::onJobProgress(QString uuid, int percent)
{
    const int row = rowForUuid(uuid);
    if (row < 0)
        return;
    if (auto *bar = qobject_cast<QProgressBar *>(m_table->cellWidget(row, 4)))
        bar->setValue(qBound(0, percent, 100));
    if (auto *statusItem = m_table->item(row, 3))
        statusItem->setText(statusToDisplay(RenderJobStatus::Rendering));
}

void RenderQueueDialog::onJobCompleted(QString uuid, bool success, QString error)
{
    Q_UNUSED(error);
    const int row = rowForUuid(uuid);
    if (row < 0)
        return;
    if (auto *statusItem = m_table->item(row, 3)) {
        statusItem->setText(statusToDisplay(success ? RenderJobStatus::Completed
                                                    : RenderJobStatus::Failed));
    }
    if (success) {
        if (auto *bar = qobject_cast<QProgressBar *>(m_table->cellWidget(row, 4)))
            bar->setValue(100);
    }
    updateButtons();
}

void RenderQueueDialog::onAllCompleted()
{
    if (m_statusLabel) {
        const int total     = m_queue ? m_queue->jobs().size() : 0;
        const int completed = m_queue ? m_queue->completedCount() : 0;
        m_statusLabel->setText(QString(QStringLiteral("%1 / %2 件完了 — 終了"))
                                   .arg(completed).arg(total));
    }
    updateButtons();
}
