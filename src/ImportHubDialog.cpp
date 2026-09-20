#include "ImportHubDialog.h"

#include "ObsScanner.h"
#include "ObsProfile.h"
#include "ObsLayout.h"
#include "AffinityPsdImporter.h"
#include "AffinityVectorImporter.h"
#include "BlenderMeshImporter.h"
#include "BlenderExrReader.h"

#include <QTabWidget>
#include <QTableWidget>
#include <QListWidget>
#include <QFileDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QCheckBox>
#include <QGroupBox>
#include <QFileInfo>
#include <QListWidgetItem>

// ---------------------------------------------------------------------------
// ctor
// ---------------------------------------------------------------------------

ImportHubDialog::ImportHubDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowFlags(Qt::Window);
    setWindowTitle(tr("Import Hub"));
    resize(760, 520);

    auto* tabs = new QTabWidget(this);

    auto* obsTab      = new QWidget;
    auto* affinityTab = new QWidget;
    auto* blenderTab  = new QWidget;

    buildObsTab(obsTab);
    buildAffinityTab(affinityTab);
    buildBlenderTab(blenderTab);

    tabs->addTab(obsTab,      tr("OBS"));
    tabs->addTab(affinityTab, tr("Affinity"));
    tabs->addTab(blenderTab,  tr("Blender"));

    auto* root = new QVBoxLayout(this);
    root->addWidget(tabs);
    auto *edlButton = new QPushButton(tr("EDL を読み込み…"), this);
    edlButton->setToolTip(tr("CMX3600 の V / A、カット / ディゾルブをプロジェクトの FPS で読み込みます。未対応行は警告として報告します。"));
    connect(edlButton, &QPushButton::clicked, this, &ImportHubDialog::onEdlBrowseClicked);
    root->addWidget(edlButton);
    auto *fcpxmlButton = new QPushButton(tr("FCPXML を読み込み…"), this);
    fcpxmlButton->setToolTip(tr("素材クリップを最大 2 トラックの新規シーケンスに読み込みます。未対応要素は警告として報告します。"));
    connect(fcpxmlButton, &QPushButton::clicked, this, &ImportHubDialog::onFcpxmlBrowseClicked);
    root->addWidget(fcpxmlButton);
    setLayout(root);
}

// ---------------------------------------------------------------------------
// Tab builders
// ---------------------------------------------------------------------------

void ImportHubDialog::buildObsTab(QWidget* tab)
{
    auto* vbox = new QVBoxLayout(tab);

    // Folder row
    {
        auto* hbox  = new QHBoxLayout;
        auto* label = new QLabel(tr("Recording folder:"));
        m_obsFolderEdit = new QLineEdit;
        m_obsFolderEdit->setPlaceholderText(tr("Select OBS recording folder…"));
        auto* browseBtn = new QPushButton(tr("Browse…"));
        m_obsScanButton = new QPushButton(tr("Scan"));
        connect(browseBtn, &QPushButton::clicked, this, &ImportHubDialog::onObsBrowseFolderClicked);
        connect(m_obsScanButton, &QPushButton::clicked, this, &ImportHubDialog::onObsScanClicked);
        hbox->addWidget(label);
        hbox->addWidget(m_obsFolderEdit, 1);
        hbox->addWidget(browseBtn);
        hbox->addWidget(m_obsScanButton);
        vbox->addLayout(hbox);
    }

    // Table
    m_obsTable = new QTableWidget(0, 5);
    m_obsTable->setHorizontalHeaderLabels({
        tr("Video File"), tr("Audio Tracks"), tr("Source Records"),
        tr("Replay Buffers"), tr("Duration (s)")
    });
    m_obsTable->horizontalHeader()->setStretchLastSection(false);
    m_obsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_obsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_obsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vbox->addWidget(m_obsTable, 1);

    // Scene JSON row
    {
        auto* hbox  = new QHBoxLayout;
        auto* label = new QLabel(tr("Scene collection JSON (optional):"));
        m_obsSceneJsonEdit = new QLineEdit;
        m_obsSceneJsonEdit->setPlaceholderText(tr("Select OBS scene collection JSON…"));
        auto* jsonBtn = new QPushButton(tr("Browse…"));
        connect(jsonBtn, &QPushButton::clicked, this, &ImportHubDialog::onObsSceneJsonClicked);
        hbox->addWidget(label);
        hbox->addWidget(m_obsSceneJsonEdit, 1);
        hbox->addWidget(jsonBtn);
        vbox->addLayout(hbox);
    }

    // Timeline button
    {
        auto* hbox = new QHBoxLayout;
        hbox->addStretch(1);
        m_obsTimelineButton = new QPushButton(tr("Place on Timeline"));
        connect(m_obsTimelineButton, &QPushButton::clicked, this, &ImportHubDialog::onObsTimelineClicked);
        hbox->addWidget(m_obsTimelineButton);
        vbox->addLayout(hbox);
    }

    tab->setLayout(vbox);
}

void ImportHubDialog::buildAffinityTab(QWidget* tab)
{
    auto* vbox = new QVBoxLayout(tab);

    // File row
    {
        auto* hbox  = new QHBoxLayout;
        auto* label = new QLabel(tr("File:"));
        m_affFileEdit = new QLineEdit;
        m_affFileEdit->setPlaceholderText(tr("Select PSD / SVG / PDF / TIFF…"));
        auto* browseBtn = new QPushButton(tr("Browse…"));
        connect(browseBtn, &QPushButton::clicked, this, &ImportHubDialog::onAffinityBrowseClicked);
        hbox->addWidget(label);
        hbox->addWidget(m_affFileEdit, 1);
        hbox->addWidget(browseBtn);
        vbox->addLayout(hbox);
    }

    // Preview + layer list side by side
    {
        auto* hbox = new QHBoxLayout;

        m_affPreviewLabel = new QLabel;
        m_affPreviewLabel->setFixedSize(240, 240);
        m_affPreviewLabel->setAlignment(Qt::AlignCenter);
        m_affPreviewLabel->setFrameShape(QFrame::Box);
        m_affPreviewLabel->setText(tr("Preview"));
        hbox->addWidget(m_affPreviewLabel);

        m_affLayerList = new QListWidget;
        m_affLayerList->setToolTip(tr("PSD layers (check to select for import)"));
        hbox->addWidget(m_affLayerList, 1);

        vbox->addLayout(hbox, 1);
    }

    // Import button
    {
        auto* hbox = new QHBoxLayout;
        hbox->addStretch(1);
        m_affImportButton = new QPushButton(tr("Import as Layer"));
        connect(m_affImportButton, &QPushButton::clicked, this, &ImportHubDialog::onAffinityImportClicked);
        hbox->addWidget(m_affImportButton);
        vbox->addLayout(hbox);
    }

    tab->setLayout(vbox);
}

void ImportHubDialog::buildBlenderTab(QWidget* tab)
{
    auto* vbox = new QVBoxLayout(tab);

    // Mesh section
    {
        auto* group = new QGroupBox(tr("Mesh"));
        auto* gvbox = new QVBoxLayout(group);

        auto* hbox = new QHBoxLayout;
        auto* label = new QLabel(tr("Mesh file:"));
        m_meshFileEdit = new QLineEdit;
        m_meshFileEdit->setPlaceholderText(tr("Select .obj / .gltf / .glb / .fbx…"));
        auto* browseBtn = new QPushButton(tr("Browse…"));
        connect(browseBtn, &QPushButton::clicked, this, &ImportHubDialog::onMeshBrowseClicked);
        hbox->addWidget(label);
        hbox->addWidget(m_meshFileEdit, 1);
        hbox->addWidget(browseBtn);
        gvbox->addLayout(hbox);

        m_meshInfoLabel = new QLabel(tr("Vertices: — Triangles: —"));
        gvbox->addWidget(m_meshInfoLabel);

        auto* btnHbox = new QHBoxLayout;
        btnHbox->addStretch(1);
        m_meshImportButton = new QPushButton(tr("Import as 3D Layer"));
        connect(m_meshImportButton, &QPushButton::clicked, this, &ImportHubDialog::onMeshImportClicked);
        btnHbox->addWidget(m_meshImportButton);
        gvbox->addLayout(btnHbox);

        group->setLayout(gvbox);
        vbox->addWidget(group);
    }

    // EXR section
    {
        auto* group = new QGroupBox(tr("EXR Sequence"));
        auto* gvbox = new QVBoxLayout(group);

        // Folder row
        {
            auto* hbox = new QHBoxLayout;
            auto* label = new QLabel(tr("EXR folder:"));
            m_exrFolderEdit = new QLineEdit;
            m_exrFolderEdit->setPlaceholderText(tr("Select folder containing EXR frames…"));
            auto* browseBtn = new QPushButton(tr("Browse…"));
            connect(browseBtn, &QPushButton::clicked, this, &ImportHubDialog::onExrBrowseFolderClicked);
            hbox->addWidget(label);
            hbox->addWidget(m_exrFolderEdit, 1);
            hbox->addWidget(browseBtn);
            gvbox->addLayout(hbox);
        }

        // Pattern + scan row
        {
            auto* hbox = new QHBoxLayout;
            auto* label = new QLabel(tr("Pattern:"));
            m_exrPatternEdit = new QLineEdit(QStringLiteral("render_####.exr"));
            auto* scanBtn = new QPushButton(tr("Scan"));
            connect(scanBtn, &QPushButton::clicked, this, &ImportHubDialog::onExrScanClicked);
            hbox->addWidget(label);
            hbox->addWidget(m_exrPatternEdit, 1);
            hbox->addWidget(scanBtn);
            gvbox->addLayout(hbox);
        }

        m_exrFrameCountLabel = new QLabel(tr("Frames: —"));
        gvbox->addWidget(m_exrFrameCountLabel);

        auto* btnHbox = new QHBoxLayout;
        btnHbox->addStretch(1);
        m_exrImportButton = new QPushButton(tr("Import as Footage"));
        connect(m_exrImportButton, &QPushButton::clicked, this, &ImportHubDialog::onExrImportClicked);
        btnHbox->addWidget(m_exrImportButton);
        gvbox->addLayout(btnHbox);

        group->setLayout(gvbox);
        vbox->addWidget(group);
    }

    vbox->addStretch(1);
    tab->setLayout(vbox);
}

// ---------------------------------------------------------------------------
// OBS slots
// ---------------------------------------------------------------------------

void ImportHubDialog::onObsBrowseFolderClicked()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select OBS Recording Folder"), QString());
    if (dir.isEmpty())
        return;
    m_obsFolderEdit->setText(dir);
}

void ImportHubDialog::onObsScanClicked()
{
    const QString folder = m_obsFolderEdit->text().trimmed();
    if (folder.isEmpty())
        return;

    m_obsGroups = obs::scan::scanFolder(folder);

    m_obsTable->setRowCount(0);
    for (const auto& g : m_obsGroups) {
        const int row = m_obsTable->rowCount();
        m_obsTable->insertRow(row);
        m_obsTable->setItem(row, 0, new QTableWidgetItem(QFileInfo(g.primaryVideoFile).fileName()));
        m_obsTable->setItem(row, 1, new QTableWidgetItem(QString::number(g.audioTrackFiles.size())));
        m_obsTable->setItem(row, 2, new QTableWidgetItem(QString::number(g.sourceRecordFiles.size())));
        m_obsTable->setItem(row, 3, new QTableWidgetItem(QString::number(g.replayBufferFiles.size())));
        m_obsTable->setItem(row, 4, new QTableWidgetItem(QString::number(g.durationSec, 'f', 2)));
    }
}

void ImportHubDialog::onObsSceneJsonClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select OBS Scene Collection"), QString(),
        tr("OBS Scene Collection (*.json)"));
    if (path.isEmpty())
        return;
    m_obsSceneJsonEdit->setText(path);
    m_obsScenes = obs::profile::loadSceneCollection(path);
}

void ImportHubDialog::onObsTimelineClicked()
{
    if (m_obsGroups.isEmpty())
        return;
    const auto placements = obs::layout::layoutToTimeline(m_obsGroups, m_obsScenes);
    emit timelineImportRequested(placements);
}

// ---------------------------------------------------------------------------
// Affinity slots
// ---------------------------------------------------------------------------

void ImportHubDialog::onAffinityBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select Affinity / Photoshop File"), QString(),
        tr("Supported files (*.psd *.svg *.pdf *.tif *.tiff);;"
           "Photoshop PSD (*.psd);;"
           "SVG (*.svg);;"
           "PDF (*.pdf);;"
           "TIFF (*.tif *.tiff)"));
    if (path.isEmpty())
        return;

    m_affFileEdit->setText(path);
    m_affLayerList->clear();
    m_psdLayers.clear();
    m_affCurrentImage = QImage();
    m_affCurrentName  = QFileInfo(path).baseName();

    const QString ext = QFileInfo(path).suffix().toLower();

    if (ext == QLatin1String("psd")) {
        const affinity::psd::PsdDocument doc = affinity::psd::loadPsd(path);
        m_psdLayers = doc.layers;

        // Populate layer list with visibility checkboxes
        for (const auto& layer : m_psdLayers) {
            auto* item = new QListWidgetItem(layer.name, m_affLayerList);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(layer.visibility ? Qt::Checked : Qt::Unchecked);
        }

        // Preview: composite visible layers (use first visible layer image as preview)
        for (const auto& layer : m_psdLayers) {
            if (layer.visibility && !layer.image.isNull()) {
                m_affCurrentImage = layer.image;
                break;
            }
        }
        if (m_affCurrentImage.isNull() && !doc.canvasSize.isEmpty()) {
            m_affCurrentImage = QImage(doc.canvasSize, QImage::Format_ARGB32);
            m_affCurrentImage.fill(Qt::transparent);
        }
    } else if (ext == QLatin1String("svg")) {
        m_affCurrentImage = affinity::vector::loadSvg(path, QSize(240, 240));
    } else if (ext == QLatin1String("pdf")) {
        m_affCurrentImage = affinity::vector::loadPdf(path, 0, 96);
    } else if (ext == QLatin1String("tif") || ext == QLatin1String("tiff")) {
        m_affCurrentImage = affinity::vector::loadTiff(path);
    }

    // Update preview label
    if (!m_affCurrentImage.isNull()) {
        const QPixmap px = QPixmap::fromImage(
            m_affCurrentImage.scaled(240, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_affPreviewLabel->setPixmap(px);
    } else {
        m_affPreviewLabel->setText(tr("No preview"));
    }
}

void ImportHubDialog::onAffinityImportClicked()
{
    if (m_affCurrentImage.isNull())
        return;

    // If PSD and a layer is selected in the list, emit that layer's image
    if (!m_psdLayers.isEmpty()) {
        const int row = m_affLayerList->currentRow();
        if (row >= 0 && row < m_psdLayers.size()) {
            const auto& layer = m_psdLayers[row];
            if (!layer.image.isNull()) {
                emit imageImportRequested(layer.image, layer.name);
                return;
            }
        }
        // Fall through to emit the current preview image if no layer selected
    }

    emit imageImportRequested(m_affCurrentImage, m_affCurrentName);
}

// ---------------------------------------------------------------------------
// Blender Mesh slots
// ---------------------------------------------------------------------------

void ImportHubDialog::onMeshBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select Mesh File"), QString(),
        tr("Mesh files (*.obj *.gltf *.glb *.fbx *.abc);;"
           "Wavefront OBJ (*.obj);;"
           "glTF (*.gltf *.glb);;"
           "FBX (*.fbx);;"
           "Alembic (*.abc)"));
    if (path.isEmpty())
        return;

    m_meshFileEdit->setText(path);
    m_meshData = blender::mesh::loadMeshFile(path);

    const int triCount = m_meshData.triangleIndices.size() / 3;
    m_meshInfoLabel->setText(
        tr("Vertices: %1   Triangles: %2")
            .arg(m_meshData.vertices.size())
            .arg(triCount));
}

void ImportHubDialog::onMeshImportClicked()
{
    if (m_meshData.vertices.isEmpty())
        return;
    emit meshImportRequested(m_meshData);
}

// ---------------------------------------------------------------------------
// Blender EXR slots
// ---------------------------------------------------------------------------

void ImportHubDialog::onExrBrowseFolderClicked()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select EXR Sequence Folder"), QString());
    if (dir.isEmpty())
        return;
    m_exrFolderEdit->setText(dir);
    m_exrFolderPath = dir;
    m_exrFrameCount = 0;
    m_exrFrameCountLabel->setText(tr("Frames: —"));
}

void ImportHubDialog::onExrScanClicked()
{
    m_exrFolderPath = m_exrFolderEdit->text().trimmed();
    const QString pattern = m_exrPatternEdit->text().trimmed();
    if (m_exrFolderPath.isEmpty() || pattern.isEmpty())
        return;

    const auto frames = blender::exr::loadExrSequence(m_exrFolderPath, pattern);
    m_exrFrameCount = frames.size();
    m_exrFrameCountLabel->setText(tr("Frames: %1").arg(m_exrFrameCount));
}

void ImportHubDialog::onExrImportClicked()
{
    const QString folder  = m_exrFolderEdit->text().trimmed();
    const QString pattern = m_exrPatternEdit->text().trimmed();
    if (folder.isEmpty() || pattern.isEmpty())
        return;
    emit exrSequenceImportRequested(folder, pattern);
}

void ImportHubDialog::onEdlBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("EDL を読み込み…"),
        QString(), tr("EDL ファイル (*.edl);;すべてのファイル (*)"));
    if (!path.isEmpty()) emit edlImportRequested(path);
}

void ImportHubDialog::onFcpxmlBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("FCPXML を読み込み"),
        QString(), tr("FCPXML ファイル (*.fcpxml);;すべてのファイル (*)"));
    if (!path.isEmpty()) emit fcpxmlImportRequested(path);
}
