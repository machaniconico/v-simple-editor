#include "MultiCamSyncDialog.h"

#include <QTableWidget>
#include <QTableWidgetItem>
#include <QProgressBar>
#include <QPushButton>
#include <QFileDialog>
#include <QInputDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QFileInfo>
#include <QFile>
#include <QTextStream>
#include <QStringList>

MultiCamSyncDialog::MultiCamSyncDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Multi-Camera Sync"));
    setWindowFlags(Qt::Window);
    setModal(false); // modeless

    m_sync = new multicam::MultiCamSync(this);

    // ---- camera table: Label / File / Offset ms ----
    m_camTable = new QTableWidget(0, 3, this);
    m_camTable->setHorizontalHeaderLabels(
        {tr("Label"), tr("File"), tr("Offset ms")});
    m_camTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_camTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_camTable->horizontalHeader()->setStretchLastSection(true);

    // ---- cut table: Time ms / Cam index ----
    m_cutTable = new QTableWidget(0, 2, this);
    m_cutTable->setHorizontalHeaderLabels(
        {tr("Time ms"), tr("Cam index")});
    m_cutTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_cutTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_cutTable->horizontalHeader()->setStretchLastSection(true);

    // ---- progress ----
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);

    // ---- buttons ----
    auto *btnAddCam   = new QPushButton(tr("Add Camera..."),  this);
    auto *btnSync     = new QPushButton(tr("Sync By Audio"),  this);
    auto *btnAddCut   = new QPushButton(tr("Add Cut..."),     this);
    auto *btnExport   = new QPushButton(tr("Export EDL..."),  this);

    connect(btnAddCam, &QPushButton::clicked,
            this, &MultiCamSyncDialog::onAddCamClicked);
    connect(btnSync, &QPushButton::clicked,
            this, &MultiCamSyncDialog::onSyncClicked);
    connect(btnAddCut, &QPushButton::clicked,
            this, &MultiCamSyncDialog::onAddCutClicked);
    connect(btnExport, &QPushButton::clicked,
            this, &MultiCamSyncDialog::onExportEdlClicked);

    connect(m_sync, &multicam::MultiCamSync::syncProgress,
            this, &MultiCamSyncDialog::onSyncProgress);
    connect(m_sync, &multicam::MultiCamSync::syncFinished,
            this, &MultiCamSyncDialog::onSyncFinished);

    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(btnAddCam);
    btnRow->addWidget(btnSync);
    btnRow->addWidget(btnAddCut);
    btnRow->addWidget(btnExport);
    btnRow->addStretch();

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(new QLabel(tr("Cameras"), this));
    mainLayout->addWidget(m_camTable);
    mainLayout->addWidget(new QLabel(tr("Angle Cuts"), this));
    mainLayout->addWidget(m_cutTable);
    mainLayout->addWidget(m_progress);
    mainLayout->addLayout(btnRow);

    resize(640, 520);
}

// ---------------------------------------------------------------------------
// table rebuilders
// ---------------------------------------------------------------------------
void MultiCamSyncDialog::rebuildCamTable()
{
    const QVector<multicam::CamSource> srcs = m_sync->sources();
    m_camTable->setRowCount(srcs.size());
    for (int i = 0; i < srcs.size(); ++i) {
        const multicam::CamSource &s = srcs[i];
        m_camTable->setItem(i, 0, new QTableWidgetItem(s.label));
        m_camTable->setItem(i, 1, new QTableWidgetItem(s.filePath));
        m_camTable->setItem(
            i, 2,
            new QTableWidgetItem(QString::number(s.offsetMs, 'f', 1)));
    }
}

void MultiCamSyncDialog::rebuildCutTable()
{
    // Re-derive cut rows from an exported EDL so we never duplicate state.
    const QString edl = m_sync->exportSwitchedEdl();
    const QStringList lines =
        edl.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    m_cutTable->setRowCount(lines.size());
    for (int i = 0; i < lines.size(); ++i) {
        // Format: "{idx}  CAM{cam}  {time}ms  {file}"
        const QStringList parts =
            lines[i].split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QString camStr;
        QString timeStr;
        if (parts.size() >= 3) {
            camStr  = parts[1]; // CAM<n>
            if (camStr.startsWith(QLatin1String("CAM")))
                camStr = camStr.mid(3);
            timeStr = parts[2]; // <time>ms
            if (timeStr.endsWith(QLatin1String("ms")))
                timeStr.chop(2);
        }
        m_cutTable->setItem(i, 0, new QTableWidgetItem(timeStr));
        m_cutTable->setItem(i, 1, new QTableWidgetItem(camStr));
    }
}

// ---------------------------------------------------------------------------
// slots
// ---------------------------------------------------------------------------
void MultiCamSyncDialog::onAddCamClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Add Camera Video"), QString(),
        tr("Video Files (*.mp4 *.mov *.mkv *.avi *.webm);;All Files (*)"));
    if (path.isEmpty())
        return;

    QVector<multicam::CamSource> srcs = m_sync->sources();

    multicam::CamSource cam;
    cam.filePath = path;
    cam.label    = QStringLiteral("Camera %1").arg(srcs.size() + 1);
    cam.offsetMs = 0.0;
    srcs.append(cam);

    m_sync->setSources(srcs);
    rebuildCamTable();
}

void MultiCamSyncDialog::onSyncClicked()
{
    m_progress->setValue(0);
    m_sync->syncByAudio();
}

void MultiCamSyncDialog::onAddCutClicked()
{
    bool ok = false;
    const double timeMs = QInputDialog::getDouble(
        this, tr("Add Cut"), tr("Time (ms):"),
        0.0, 0.0, 1.0e9, 1, &ok);
    if (!ok)
        return;

    const int maxCam = qMax(0, m_sync->sources().size() - 1);
    const int camIndex = QInputDialog::getInt(
        this, tr("Add Cut"), tr("Camera index:"),
        0, 0, maxCam, 1, &ok);
    if (!ok)
        return;

    m_sync->addAngleCut(timeMs, camIndex);
    rebuildCutTable();
}

void MultiCamSyncDialog::onExportEdlClicked()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Switched EDL"), QStringLiteral("multicam.edl"),
        tr("EDL Files (*.edl *.txt);;All Files (*)"));
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;

    QTextStream out(&f);
    out << m_sync->exportSwitchedEdl();
    f.close();
}

void MultiCamSyncDialog::onSyncProgress(int percent)
{
    m_progress->setValue(percent);
}

void MultiCamSyncDialog::onSyncFinished()
{
    m_progress->setValue(100);
    rebuildCamTable(); // refresh offsets computed by syncByAudio
}
