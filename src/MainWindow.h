#pragma once

#include <QMainWindow>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QSplitter>
#include <QFileDialog>
#include <QSettings>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QLabel>
#include <QShowEvent>
#include <QColor>
#include <QRectF>
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
#include "LayerCompositor.h"
#include "TransformAnimator.h"
#include "MaskSystem.h"
#include "ParticleSystem.h"
#include "Camera3D.h"
#include "Expression.h"
#include "ShapeLayer.h"
#include "TextAnimator.h"
#include "TrackerLink.h"
#include "Precompose.h"
#include "Rotoscope.h"
#include "WarpDistortion.h"
#include "ShortcutEditor.h"
#include "RecentFiles.h"
#include "ShaderEffect.h"
#include "VSTHost.h"
#include "PythonScript.h"
#include "NetworkRender.h"
#include "RemotionExport.h"
#include "WelcomeWidget.h"

class VideoPlayer;
class Timeline;
class ExportDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

public slots:
    // Used by main.cpp when the app is launched with a file argument.
    // Loads the given file as a media clip (same code path as File > Open).
    void testLoadFile(const QString &filePath);

    // Used by main.cpp --play flag: start video playback.
    void testStartPlayback();

private slots:
    void newProject();
    void editProjectSettings();
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
    void exportTextOverlays();
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
    void addShapeLayer();
    void addParticleEffect();
    void addTextAnimation();
    void editTransformKeyframes();
    void addMask();
    void applyWarpEffect();
    void editExpressions();
    void precomposeSelected();
    void showResourceGuide();
    void stabilizeVideo();
    void applyLut();
    void manageLuts();
    void toggleProxyMode();
    void generateProxies();
    // Modal dialog driving the seekbar-left proxy button. Lets the user
    // flip ProxyManager's proxy-mode flag and pick the preview divisor
    // from the same place. Replaces the divisor-only cycle handler that
    // used to live inside VideoPlayer.
    void openProxySettings();
    void openProxyManagement();
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

    // Phase 14 slots
    void editShortcuts();
    void openRecentFile(const QString &filePath);
    void applyShaderEffect();
    void manageShaderEffects();
    void openVSTPlugins();
    void openScriptConsole();
    void openNetworkRender();
    void exportToRemotion();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void setupMenuBar();
    void setupToolBar();
    void setupUI();
    void setupRecentFiles();
    void setupStatusBarWidgets();
    void saveWindowState();
    void restoreWindowState();
    void showWelcomeScreen();
    void hideWelcomeScreen();
    void loadMediaFile(const QString &filePath, bool addToTimeline, const QString &statusPrefix);
    void updateStatusInfo();
    void updateEditActions();
    void applyProjectConfig(const ProjectConfig &config);
    void updateTitle();

    VideoPlayer *m_player;
    Timeline *m_timeline;
    class ProxyProgressDialog *m_proxyDialog = nullptr;
    class ColorGradingPanel *m_colorGradingPanel = nullptr;
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
    LayerCompositor m_layerCompositor;
    PrecomposeManager m_precomposeManager;

    // Phase 14
    RecentFilesManager *m_recentFilesManager = nullptr;
    RecentFilesMenu *m_recentFilesMenu = nullptr;
    ScriptEngine *m_scriptEngine = nullptr;
    ScriptManager m_scriptManager;
    NetworkRenderServer m_networkRenderServer;

    // UI/UX (Phase 14b)
    QSplitter *m_mainSplitter = nullptr;
    WelcomeWidget *m_welcomeWidget = nullptr;
    QLabel *m_statusResolution = nullptr;
    QLabel *m_statusFps = nullptr;
    QLabel *m_statusDuration = nullptr;
    QLabel *m_statusTheme = nullptr;
    bool m_hasContent = false;
    bool m_autoSaveStarted = false;

    // Text tool mode + right-side property panel (Adobe-style text tool).
    // m_previewSplitter wraps [m_player | m_toolPropertyStack] horizontally
    // and is the widget inserted into m_mainSplitter's top half.
    QSplitter *m_previewSplitter = nullptr;
    class QStackedWidget *m_toolPropertyStack = nullptr;
    QAction *m_textToolAction = nullptr;
    bool m_textToolActive = false;
    class QLineEdit *m_textToolLineEdit = nullptr;
    class QSpinBox *m_textToolSizeSpin = nullptr;
    class QPushButton *m_textToolColorButton = nullptr;
    class QDoubleSpinBox *m_textToolStartSpin = nullptr;
    class QDoubleSpinBox *m_textToolEndSpin = nullptr;
    QColor m_textToolColor;
    class QCheckBox *m_textToolGradientCheck = nullptr;
    class QPushButton *m_textToolGradientStartBtn = nullptr;
    class QPushButton *m_textToolGradientEndBtn = nullptr;
    class QDoubleSpinBox *m_textToolGradientAngleSpin = nullptr;
    class QComboBox *m_textToolGradientTypeCombo = nullptr;
    class QDoubleSpinBox *m_textToolGradientMidSpin = nullptr;
    class QCheckBox *m_textToolGradientReverseCheck = nullptr;
    QColor m_textToolGradientStart;
    QColor m_textToolGradientEnd;
    // Illustrator-style multi-stop gradient editor and per-stop controls.
    class GradientStopBar *m_textToolStopBar = nullptr;
    class QPushButton *m_textToolStopColorBtn = nullptr;
    class QDoubleSpinBox *m_textToolStopOpacitySpin = nullptr;
    class QDoubleSpinBox *m_textToolStopPosSpin = nullptr;
    class QPushButton *m_textToolGradientPresetSaveBtn = nullptr;
    class QPushButton *m_textToolGradientPresetLoadBtn = nullptr;
    class QCheckBox *m_textToolOutlineCheck = nullptr;
    class QPushButton *m_textToolOutlineColorBtn = nullptr;
    class QSpinBox *m_textToolOutlineWidthSpin = nullptr;
    QColor m_textToolOutlineColor;
    // Last drag rectangle on the preview in normalized 0.0–1.0 widget-relative
    // coordinates; populated by VideoPlayer::textRectRequested and consumed by
    // the 適用 button.
    QRectF m_textToolPendingRect;
    bool m_textToolHasPendingRect = false;

    void setupToolPropertyPanel();
    void onTextToolToggled(bool checked);
    void onTextRectRequested(const QRectF &normalizedRect);
    void onTextInlineCommitted(const QString &text, const QRectF &normalizedRect);
    void onTextOverlayEditCommitted(int overlayIndex, const QString &newText);
    void applyTextToolOverlay();
    void pushTextToolStyleToPreview();
};
