#pragma once
#include <QDialog>
#include <QImage>

#include "ObsScanner.h"
#include "ObsProfile.h"
#include "ObsLayout.h"
#include "AffinityPsdImporter.h"
#include "BlenderMeshImporter.h"

class QTabWidget;
class QTableWidget;
class QListWidget;
class QLineEdit;
class QPushButton;
class QLabel;

class ImportHubDialog : public QDialog {
    Q_OBJECT
public:
    explicit ImportHubDialog(QWidget* parent = nullptr);

signals:
    void fcpxmlImportRequested(const QString& filePath);
    void edlImportRequested(const QString& filePath);
    void timelineImportRequested(const QList<obs::layout::TimelineClipPlacement>& placements);
    void imageImportRequested(const QImage& image, const QString& name);
    void meshImportRequested(const blender::mesh::MeshData& meshData);
    void exrSequenceImportRequested(const QString& folderPath, const QString& pattern);

private slots:
    void onFcpxmlBrowseClicked();
    void onEdlBrowseClicked();
    // OBS tab
    void onObsBrowseFolderClicked();
    void onObsScanClicked();
    void onObsSceneJsonClicked();
    void onObsTimelineClicked();

    // Affinity tab
    void onAffinityBrowseClicked();
    void onAffinityImportClicked();

    // Blender Mesh section
    void onMeshBrowseClicked();
    void onMeshImportClicked();

    // Blender EXR section
    void onExrBrowseFolderClicked();
    void onExrScanClicked();
    void onExrImportClicked();

private:
    void buildObsTab(QWidget* tab);
    void buildAffinityTab(QWidget* tab);
    void buildBlenderTab(QWidget* tab);

    // OBS tab widgets
    QLineEdit*    m_obsFolderEdit       = nullptr;
    QPushButton*  m_obsScanButton       = nullptr;
    QTableWidget* m_obsTable            = nullptr;
    QLineEdit*    m_obsSceneJsonEdit    = nullptr;
    QPushButton*  m_obsTimelineButton   = nullptr;

    // Affinity tab widgets
    QLineEdit*    m_affFileEdit         = nullptr;
    QLabel*       m_affPreviewLabel     = nullptr;
    QListWidget*  m_affLayerList        = nullptr;
    QPushButton*  m_affImportButton     = nullptr;

    // Blender Mesh widgets
    QLineEdit*    m_meshFileEdit        = nullptr;
    QLabel*       m_meshInfoLabel       = nullptr;
    QPushButton*  m_meshImportButton    = nullptr;

    // Blender EXR widgets
    QLineEdit*    m_exrFolderEdit       = nullptr;
    QLineEdit*    m_exrPatternEdit      = nullptr;
    QLabel*       m_exrFrameCountLabel  = nullptr;
    QPushButton*  m_exrImportButton     = nullptr;

    // State
    QList<obs::scan::RecordingGroup>          m_obsGroups;
    QList<obs::profile::SceneInfo>            m_obsScenes;
    QList<affinity::psd::PsdLayer>            m_psdLayers;
    QImage                                    m_affCurrentImage;
    QString                                   m_affCurrentName;
    blender::mesh::MeshData                   m_meshData;
    QString                                   m_exrFolderPath;
    int                                       m_exrFrameCount = 0;
};
