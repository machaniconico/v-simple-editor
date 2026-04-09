#pragma once

#include <QMainWindow>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QSplitter>
#include <QFileDialog>
#include "ProjectSettings.h"
#include "Exporter.h"
#include "ProjectFile.h"
#include "AutoEdit.h"
#include "ThemeManager.h"
#include "MultiCam.h"
#include "MotionTracker.h"
#include "NoiseReduction.h"
#include "SubtitleGenerator.h"
#include "EffectPreset.h"
#include "ResourceGuide.h"
#include "AutoSave.h"
#include "LutImporter.h"
#include "ProxyManager.h"
#include "VideoStabilizer.h"
#include "SpeedRamp.h"
#include "AudioEQ.h"
#include "TimelineMarker.h"
#include "RenderQueue.h"
#include "ScreenRecorder.h"
#include "AIHighlight.h"

class VideoPlayer;
class Timeline;
class ExportDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void newProject();
    void openFile();
    void saveProject();
    void saveProjectAs();
    void openProject();
    void exportVideo();
    void splitClip();
    void deleteClip();
    void rippleDelete();
    void copyClip();
    void pasteClip();
    void undoAction();
    void redoAction();
    void toggleSnap();
    void zoomIn();
    void zoomOut();
    void addVideoTrack();
    void addAudioTrack();
    void markIn();
    void markOut();
    void setClipSpeed();
    void addTextOverlay();
    void manageTextOverlays();
    void importSubtitles();
    void saveTextTemplate();
    void addTransition();
    void addImageOverlay();
    void addPip();
    void setClipVolume();
    void addBgm();
    void toggleMute();
    void toggleSolo();
    void colorCorrection();
    void videoEffects();
    void pluginEffects();
    void editKeyframes();
    void autoSilenceDetect();
    void autoJumpCut();
    void autoSceneDetect();
    void changeTheme();
    void multiCamSetup();
    void multiCamSwitch();
    void motionTrackSetup();
    void audioNoiseDenoise();
    void videoNoiseDenoise();
    void generateSubtitles();
    void applyEffectPreset();
    void saveEffectPreset();
    void manageEffectPresets();
    void showResourceGuide();
    void stabilizeVideo();
    void applyLut();
    void manageLuts();
    void toggleProxyMode();
    void generateProxies();
    void setSpeedRamp();
    void audioEqualizer();
    void audioEffects();
    void addMarker();
    void showMarkers();
    void exportChapters();
    void openRenderQueue();
    void startScreenRecording();
    void stopScreenRecording();
    void analyzeHighlights();
    void about();

private:
    void setupMenuBar();
    void setupToolBar();
    void setupUI();
    void updateEditActions();
    void applyProjectConfig(const ProjectConfig &config);
    void updateTitle();

    VideoPlayer *m_player;
    Timeline *m_timeline;
    QStringList m_supportedFormats;
    ProjectConfig m_projectConfig;

    QAction *m_splitAction;
    QAction *m_deleteAction;
    QAction *m_rippleDeleteAction;
    QAction *m_copyAction;
    QAction *m_pasteAction;
    QAction *m_undoAction;
    QAction *m_redoAction;
    QAction *m_snapAction;
    Exporter *m_exporter;
    QString m_projectFilePath; // current .veditor file
    MultiCamSession *m_multiCam = nullptr;
    MotionTracker *m_motionTracker = nullptr;
    NoiseReduction *m_noiseReduction = nullptr;
    SubtitleGenerator *m_subtitleGen = nullptr;
    AutoSave *m_autoSave = nullptr;
    VideoStabilizer *m_stabilizer = nullptr;
    SpeedRamp *m_speedRamp = nullptr;
    AudioEQProcessor *m_audioEQ = nullptr;
    MarkerManager m_markerManager;
    RenderQueue *m_renderQueue = nullptr;
    ScreenRecorder *m_screenRecorder = nullptr;
    AIHighlight *m_aiHighlight = nullptr;
};
