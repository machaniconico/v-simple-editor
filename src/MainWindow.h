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
#include <QHash>
#include <QVector>
#include <QPair>
#include <QRectF>
#include <QString>
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
#include "ClipExpressionBindings.h"
#include "WiggleTransform.h"
#include "ShapeLayer.h"
#include "TextAnimator.h"
#include "PathText.h"
#include "Text3DLayer.h"
#include "TextPathWarp.h"
#include "TextMaskReveal.h"
#include "VariableFontAxis.h"
#include "MographText.h"
#include "TrackerLink.h"
#include "Precompose.h"
#include "Rotoscope.h"
#include "WarpDistortion.h"

#include "RecentFiles.h"
#include "ShaderEffect.h"
#include "VSTHost.h"
#include "PythonScript.h"
#include "NetworkRender.h"
#include "RemotionExport.h"
#include "WelcomeWidget.h"
#include "HistoryDockWidget.h"
#include "SmartReframe.h"
#include "LoudnessAnalyzer.h"
#include "SubtitleTrackRenderer.h"
#include "AudioDucking.h"
#include "ExportDialog.h"          // brings in HDRSettings
#include "AIProcessingDialog.h"    // brings in AIProcessingSettings

class VideoPlayer;
class Timeline;
class ExportDialog;
class BrushAnimation;
class RotoToolsDialog;
class TimeRemapDialog;
class FavoritesEditDialog;
class Text3DExtrusionDialog;
class ExpressionBindingDialog;
class CameraMotionDialog;
class SceneCutDialog;
class AudioDuckingDialog;
class ProjectCollectorDialog;
class HDRSettingsDialog;
class AIProcessingDialog;
class PluginBrowserDialog;
class AIMaskDialog;
class PlanarTrackerDialog;
class AudioClipEditor;
class ShortcutCustomizeDialog;
class SocialExportDialog;
class CaptionEditorDialog;
class MobileExportDialog;
class ImportHubDialog;
class VimeoUploadDialog;
class TwitchStreamDialog;
class CloudRenderDialog;
class SmartEditDialog;

// US-INT-2: Sprint 21 — platform expansion / mastering / batch export.
class XVideoDialog;
class InstagramPublishDialog;
class ProjectTemplateDialog;
class LoudnessMasterDialog;
class HdrGradingDialog;
class MultiCamSyncDialog;
class BatchExportDialog;

// US-INT-2: Sprint 22 — keying / restoration / animated export / easing /
// subtitle translation / lower-third / watermark.
class ChromaKeyRefineDialog;
class AudioRestorationDialog;
class AnimatedExportDialog;
class EasingCurveEditorDialog;
class SubtitleTranslatorDialog;
class LowerThirdDialog;
class WatermarkDialog;

// Sprint 17/18/19 — final integration (US-INT-3)
class YoutubeUploadDialog;
class CommentsDockWidget;
class CollabHistoryDialog;
class ColorMatchDialog;

namespace youtube {
namespace oauth   { class AuthClient; }
namespace manager { class Manager; }
}

namespace vimeo {
namespace oauth   { class AuthClient; }
namespace manager { class Manager; }
}

namespace frameio {
namespace importer { class FrameIoImporter; }
}

namespace cloudrender { class Client; }

namespace collab {
struct CommentTrack;
namespace history { class HistoryLog; }
}

namespace shortcut { class ShortcutManager; }

namespace voiceover {
class VoiceOverDialog;
}

namespace effectctrl {
class EffectClipboard;
class EffectControlsPanel;
struct ClipMotion;
}
class PasteAttributesDialog;
class LoudnessPanel;
class QDockWidget;
class NodeCanvasWidget;
class NodePropertiesPanel;
class NodeGraph;
class NodeEvaluator;
class ParticleEffectDialog;
class VfxControlsPanel;

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

    // US-SNS-7: Smart Reframe dialog + analysis
    void openSmartReframe();

    // US-SNS-7: Render subtitle track from existing segments
    void renderSubtitleTrack();

    // US-SNS-7: Measure timeline master loudness from the dock panel.
    void measureLoudness();

    // US-SNS-7: Loudness normalization slot
    void applyLoudnessNormalize(double targetLUFS, double gainDb);

public:
    // Current playhead position in seconds. Used by EffectControlsPanel
    // for keyframe insertion at the current timeline position.
    double currentPlayheadSeconds() const;

signals:
    void playheadSecondsChanged(double seconds);

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
    void applyDefaultTransition();
    void editDefaultTransition();
    void addImageOverlay();
    void addPip();
    void setClipVolume();
    void addBgm();
    void toggleMute();
    void toggleSolo();
    void colorCorrection();
    void videoEffects();
    void pluginEffects();
    void trackMotion(); // US-FEAT-D: motion tracking UI
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
    void addBrushAnimation();
    void editTransformKeyframes();
    void addMask();
    void applyWarpEffect();
    void editExpressions();
    void precomposeSelected();
    void showResourceGuide();
    void stabilizeVideo();
    void applyLut();
    void loadLutCubeFile();
    void clearLutIntensity();
    void manageLuts();
    void toggleProxyMode();
    void generateProxies();
    // Modal dialog driving the seekbar-left proxy button. Lets the user
    // flip ProxyManager's proxy-mode flag and pick the preview divisor
    // from the same place. Replaces the divisor-only cycle handler that
    // used to live inside VideoPlayer.
    void openProxySettings();
    void openLoudnessSettings();
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
    void openRecentFile(const QString &filePath);
    void applyShaderEffect();
    void manageShaderEffects();
    void openVSTPlugins();
    void openScriptConsole();
    void openNetworkRender();
    void exportToRemotion();

    // Effects + audio shortcuts
    void applySharpenEffect();
    void applyMosaicEffect();
    void applyChromaKeyEffect();
    void openMasterCompressor();
    void openAutoDuckSettings();

    // Audio meter context menu handlers
    void onMeterRequestEqPresetMenu(int trackIdx, QPoint globalPos);
    void onMeterRequestCompressorDialog();
    void onMeterRequestAutoDuckDialog();
    void onMeterRequestNormalize(int trackIdx, double gainDb);
    void onMeterRequestNormalizeAll();
    void onMeterRequestResetAllMeters();

    // Consolidation slots — wire panels and dialogs implemented in
    // earlier sprint stories into the menu bar.
    void openEqualizerPanel();
    void openCompressorPanel();
    void openReverbPanel();
    void openNoiseReductionPanel();
    void openVoiceOverDialog();
    void openTitlePresetDialog();
    void openMultiCamDialog();
    void copyEffects();
    void pasteEffects();
    void pasteAttributes();
    // Receives MultiCamDialog::applyToTimeline. Replaces V1 + A1 with the
    // EDL encoded in MultiCamProject::switches (with a confirm prompt when
    // V1 is non-empty). No-op on Cancel; missing-file segments are skipped
    // with a status-bar warning rather than crashing.
    void onMultiCamApplyToTimeline(const MultiCamProject &project);
    void openRenderQueueDialog();
    void openSceneDetector();
    void runMotionStabilizer();
    void addAdjustmentLayerCmd();
    void openSpeedRampDialog();
    void addQuickMarker();
    void addColoredMarker();
    void jumpToNextMarker();
    void jumpToPrevMarker();

    // US-AETEXT-12: MainWindow Text menu integration
    void addPathText();
    void addRangeSelector();
    void addWigglySelector();
    void addSourceTextKeyframe();
    void addAnimationPreset();
    void add3DText();
    void addMaskTextReveal();
    void addBendTextWarp();
    void changeTextScope();
    void addVariableFontAxis();
    void addMographTemplate();
    void openRotoToolsDialog();
    void openTimeRemapDialog();
    void configureTrackMatte();

    // US-3D-11: motion-graphics sprint — 4 new menu actions
    void open3DExtrudedText();
    void editClipExpressionBindings();
    void editClipWiggle();
    void openCameraMotionDialog();

    // US-HW-10: Sprint 9 hardware/perf — 3 new menu actions
    void onSceneCutDetect();
    void onAudioDuckingSettings();
    void onCollectProject();

    // US-EXT-10: Sprint 10 pro extensions — 3 new menu actions
    void onHDRSettings();
    void onAIProcessing();
    void onPluginBrowser();

    // US-WF-D: Sprint 11 workflow — 3 new menu actions
    void openAIMaskDialog();
    void openAudioClipEditorDialog();
    void runMagneticTimelineDemo();

    // US-PT-B: Sprint 15 — Planar (4-corner) tracker dialog
    void openPlanarTrackerDialog();

    // US-SC-B: Sprint 12 — ショートカット設定ダイアログ
    void openShortcutCustomizeDialog();

    // US-SC2-B: Sprint 13 — SNS 向けエクスポートダイアログ
    void openSocialExportDialog();

    // US-CAP-B: Sprint 14 — 字幕エディタダイアログ
    void openCaptionEditorDialog();

    // US-INT-1: Sprint 16 — モバイルデバイス向けエクスポートダイアログ
    void onMobileExport();

    // US-INT-1: Sprint 16 — 外部ツール取り込みハブ (OBS/Affinity/Blender)
    void onImportHub();

    // US-INT-3: Sprint 17/18/19 — final integration menu slots.
    void onYoutubeUpload();   // Sprint 17 (US-YT-4): YouTube アップロード dialog
    void onCommentsPanel();   // Sprint 18 (US-COL-2): comment dock widget
    void onCollabHistory();   // Sprint 18 (US-COL-4): version history dialog
    void onColorMatch();      // Sprint 19 (US-CMA-3): auto color match dialog

    // US-INT-2: Sprint 20 — platform integration / export / smart-edit / cloud render.
    void openVimeoUploadDialog();
    void openTwitchStreamDialog();
    void openFrameIoImportDialog();
    void openDavinciExportDialog();
    void openFcpxmlExportDialog();
    void openSmartEditDialog();
    void openCloudRenderDialog();

    // US-INT-2: Sprint 21 — platform expansion / mastering / batch export.
    void openXVideoDialog();
    void openInstagramDialog();
    void openProjectTemplateDialog();
    void openLoudnessDialog();
    void openHdrDialog();
    void openMultiCamSyncDialog();
    void openBatchExportDialog();

    // US-INT-2: Sprint 22 — keying / restoration / animated export / easing /
    // subtitle translation / lower-third / watermark.
    void openChromaKeyDialog();
    void openAudioRestoreDialog();
    void openAnimExportDialog();
    void openEasingEditorDialog();
    void openSubtitleTranslatorDialog();
    void openLowerThirdDialog();
    void openWatermarkDialog();

    // User-customizable "お気に入り" menu — opens FavoritesEditDialog, then
    // persists the chosen action ids to QSettings and rebuilds the menu.
    void editFavorites();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void setupMenuBar();
    // Beginner-friendly Japanese hover help for menu items. Iterates
    // m_menuHelpEntries and either sets each QAction's tooltip to its
    // description (enabled) or clears it (disabled). Persisted via
    // QSettings("VSimpleEditor","Preferences") key "showMenuHints".
    void applyMenuHelpTooltips(bool enabled);
    // Rebuilds the dynamic part of the お気に入り menu from the persisted
    // QSettings("VSimpleEditor","Preferences")/favoriteActions id list.
    // For each known id, adds a fresh lightweight proxy QAction that mirrors
    // the original action's icon/text and forwards trigger() to it. Always
    // re-appends a separator + the 「お気に入りを編集...」 action at the bottom.
    void rebuildFavoritesMenu();
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
    void populateProjectData(ProjectData &data);
    void applyLoadedProjectData(const ProjectData &data, const QString &filePath);
    void collectAudioState(ProjectData &data);
    void applyAudioState(const ProjectData &data);
    static QString brushClipId(int trackIdx, int clipIdx);
    static QString particleClipKey(const ClipInfo &clip);
    bool selectedVideoClipRef(int &trackIdx, int &clipIdx, ClipInfo *clip = nullptr) const;
    double clipTimelineStartSeconds(int trackIdx, int clipIdx) const;
    double clipSourceTimeAtPlayheadSeconds(int trackIdx, int clipIdx, const ClipInfo &clip) const;
    QImage decodeClipFrameAtSourceTime(const ClipInfo &clip, double sourceTimeSeconds) const;
    QImage decodeClipFrameByIndex(const ClipInfo &clip, int sourceFrameIndex, double sourceFps) const;
    void refreshSpecialClipPreview();
    QImage buildSpecialClipComposite(double timelineSeconds) const;
    QImage applyStoredRotoData(const QString &clipId, const QImage &frame, int sourceFrameIndex) const;
    void setBrushAnimationEntries(const QVector<BrushAnimationEntry> &entries);
    void upsertBrushAnimationEntry(const BrushAnimationEntry &entry);
    BrushAnimation *materializeBrushAnimation(const QString &clipId);
    void syncBrushAnimationPreviewForClip(int trackIdx, int clipIdx);
    void applyVfxProjectState(const ProjectVfxState &state);

    VideoPlayer *m_player;
    Timeline *m_timeline;
    class ProxyProgressDialog *m_proxyDialog = nullptr;
    class ColorGradingPanel *m_colorGradingPanel = nullptr;
    effectctrl::EffectControlsPanel *m_effectControlsPanel = nullptr;
    VfxControlsPanel *m_vfxControlsPanel = nullptr;
    QStringList m_supportedFormats;
    ProjectConfig m_projectConfig;
    QVector<BrushAnimationEntry> m_brushAnimationEntries;
    QHash<QString, ParticleEmitterConfig> m_particleClipConfigs;
    QHash<QString, BrushAnimation *> m_liveBrushAnimations;
    QHash<QString, RotoClipEntry> m_rotoClipEntries;
    QHash<QString, TimeRemapClipEntry> m_timeRemapClipEntries;
    QHash<QString, TrackMatteClipEntry> m_trackMatteClipEntries;
    // US-3D-11: per-clip motion-graphics sidecars, keyed by "trackIdx:clipIdx"
    QHash<QString, QJsonObject> m_text3DClipConfigs;          // Text3DLayer::toJson() blobs
    QHash<QString, exprbind::ClipExpressionBindings> m_clipExpressionBindings;
    QHash<QString, wiggle::WiggleParams> m_clipWiggleParams;
    Camera3D m_projectCamera;                                 // single per-project camera
    int m_selectedVideoTrackIndex = -1;
    int m_selectedVideoClipIndexTracked = -1;

    // US-HW-10: project-level audio ducking state, persisted via ProjectFile.
    DuckingParams m_duckingParams;
    bool          m_duckingEnabled = false;

    // US-EXT-10: project-level HDR + AI processing state, persisted via
    // ProjectFile. HDRSettings is defined in ExportDialog.h; AIProcessingSettings
    // is defined in AIProcessingDialog.h.
    HDRSettings           m_hdrSettings;
    AIProcessingSettings  m_aiSettings;

    // US-AETEXT-12: AE text feature objects
    QVector<PathText *> m_pathTexts;
    QVector<Text3DLayer *> m_text3DLayers;
    QVector<TextMaskReveal *> m_textMaskReveals;
    QVector<TextPathWarp *> m_textPathWarps;
    QVector<VariableFontAxis *> m_variableFontAxes;
    QVector<MographText *> m_mographTexts;

    QAction *m_trackMotionAction = nullptr; // US-FEAT-D: motion tracking UI
    class QSlider *m_lutIntensitySlider = nullptr; // LUT intensity slider (0..100)
    QAction *m_splitAction;
    QAction *m_deleteAction;
    QAction *m_rippleDeleteAction;
    QAction *m_copyAction;
    QAction *m_pasteAction;
    QAction *m_copyEffectsAction = nullptr;
    QAction *m_pasteEffectsAction = nullptr;
    QAction *m_pasteAttributesAction = nullptr;
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

    // US-SNS-7: SNS pack members
    SmartReframe m_smartReframe;
    QVector<SubtitleSegment> m_subtitleSegments;
    SubtitleStyle m_subtitleStyle;
    LoudnessPanel *m_loudnessPanel = nullptr;
    QDockWidget *m_loudnessDock = nullptr;
    bool m_loudnessMeasureRunning = false;
    QDockWidget *m_vfxControlsDock = nullptr;
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

    // Audio meters dock
    QDockWidget *m_audioMetersDock = nullptr;
    QList<QMetaObject::Connection> m_meterConnections;
    QList<class AudioMeterWidget *> m_audioMeterWidgets;
    class AudioMeterWidget *m_masterMeter = nullptr;
    void rebuildAudioMeters();

    // History dock
    HistoryDockWidget *m_historyDock = nullptr;
    QAction *m_vfxControlsAction = nullptr;

    // Beginner-friendly hover help: pairs of (menu action, Japanese
    // explanation). Populated in setupMenuBar() as each action is created;
    // applied/cleared by applyMenuHelpTooltips() and toggled via the
    // "メニューの説明を表示" preference.
    QVector<QPair<QAction *, QString>> m_menuHelpEntries;

    // User-customizable "お気に入り" menu support.
    // FavoritableAction::id is a STABLE string (e.g. "file.new", "edit.split")
    // — never the translated text — so the persisted favorites list survives
    // UI-text changes. label = current display text (shown in the editor
    // dialog), menuPath = parent menu title used for grouping in the dialog,
    // action = the real menu QAction (the proxy added to the お気に入り menu
    // forwards trigger() to it). Populated in setupMenuBar() alongside
    // m_menuHelpEntries.
    struct FavoritableAction {
        QString id;
        QString label;
        QString menuPath;
        QAction *action = nullptr;
    };
    QVector<FavoritableAction> m_favoritableActions;
    QMenu *m_favoritesMenu = nullptr;
    QAction *m_editFavoritesAction = nullptr;

    // Consolidation: docks for the per-track audio panels (EQ /
    // Compressor / Reverb / Noise Reduction). Created on first use and
    // re-used on subsequent menu invocations so toggling the action
    // shows/hides the same dock.
    QDockWidget *m_equalizerDock = nullptr;
    QDockWidget *m_compressorDock = nullptr;
    QDockWidget *m_reverbDock = nullptr;
    QDockWidget *m_noiseReductionDock = nullptr;

    // Modeless render queue dialog — kept alive between invocations so
    // running jobs persist when the user closes the window.
    class RenderQueueDialog *m_renderQueueDialog = nullptr;

    // US-WF-D: Sprint 11 workflow dialogs — kept alive between invocations.
    // m_aiMaskDialog hosts AIMask preview & generate UI; m_audioClipEditorDialog
    // is a lightweight QDialog wrapper around an AudioClipEditor widget.
    class AIMaskDialog *m_aiMaskDialog = nullptr;
    class QDialog *m_audioClipEditorDialog = nullptr;

    // US-PT-B: Sprint 15 — Planar (4-corner) tracker dialog, kept alive between invocations.
    PlanarTrackerDialog *m_planarTrackerDialog = nullptr;

    // US-SC-B: Sprint 12 — ショートカット管理 (preset/カスタムバインド)。
    // m_shortcutManager は registerAction で登録した QAction* の弱参照を保ち、
    // m_shortcutCustomizeDialog はモードレスダイアログとして再利用する。
    shortcut::ShortcutManager *m_shortcutManager = nullptr;
    ShortcutCustomizeDialog *m_shortcutCustomizeDialog = nullptr;
    void registerCoreShortcuts();

    // US-SC2-B: Sprint 13 — SNS 向けエクスポートダイアログ (modeless, 再利用)
    SocialExportDialog *m_socialExportDialog = nullptr;

    // US-CAP-B: Sprint 14 — 字幕エディタダイアログ (modeless, 再利用)
    CaptionEditorDialog *m_captionEditorDialog = nullptr;

    // US-INT-1: Sprint 16 — モバイルエクスポート / 取り込みハブ (modeless, 再利用)
    MobileExportDialog *m_mobileExportDialog = nullptr;
    ImportHubDialog *m_importHubDialog = nullptr;

    // US-INT-3: Sprint 17/18/19 — final integration members (modeless, 再利用 / lazy-create)
    YoutubeUploadDialog          *m_youtubeUploadDialog = nullptr;
    CommentsDockWidget           *m_commentsDock        = nullptr;
    CollabHistoryDialog          *m_collabHistoryDialog = nullptr;
    ColorMatchDialog             *m_colorMatchDialog    = nullptr;
    youtube::oauth::AuthClient   *m_youtubeOAuth        = nullptr;
    youtube::manager::Manager    *m_youtubeManager      = nullptr;
    collab::CommentTrack         *m_commentTrack        = nullptr;
    collab::history::HistoryLog  *m_collabHistoryLog    = nullptr;

    // US-INT-2: Sprint 20 — platform expansion / export / smart-edit / cloud render.
    VimeoUploadDialog                  *m_vimeoUploadDialog;
    TwitchStreamDialog                 *m_twitchStreamDialog;
    frameio::importer::FrameIoImporter *m_frameIoImporter;
    cloudrender::Client                *m_cloudRenderClient;
    CloudRenderDialog                  *m_cloudRenderDialog;
    SmartEditDialog                    *m_smartEditDialog;
    vimeo::oauth::AuthClient           *m_vimeoOAuth;
    vimeo::manager::Manager            *m_vimeoManager;

    // US-INT-2: Sprint 21 — platform expansion / mastering / batch export.
    XVideoDialog                       *m_xVideoDialog;
    InstagramPublishDialog             *m_instagramDialog;
    ProjectTemplateDialog              *m_projectTemplateDialog;
    LoudnessMasterDialog               *m_loudnessDialog;
    HdrGradingDialog                   *m_hdrDialog;
    MultiCamSyncDialog                 *m_multiCamSyncDialog;
    BatchExportDialog                  *m_batchExportDialog;

    // US-INT-2: Sprint 22 — keying / restoration / animated export / easing /
    // subtitle translation / lower-third / watermark.
    ChromaKeyRefineDialog              *m_chromaKeyDialog;
    AudioRestorationDialog             *m_audioRestoreDialog;
    AnimatedExportDialog               *m_animExportDialog;
    EasingCurveEditorDialog            *m_easingEditorDialog;
    SubtitleTranslatorDialog           *m_subtitleTranslatorDialog;
    LowerThirdDialog                   *m_lowerThirdDialog;
    WatermarkDialog                    *m_watermarkDialog;

    // Voice-over recording
    voiceover::VoiceOverDialog *m_voiceOverDialog = nullptr;

    // US-NODE-9: Node compositing mode
    NodeGraph *m_activeNodeGraph = nullptr;
    NodeCanvasWidget *m_nodeCanvas = nullptr;
    NodePropertiesPanel *m_nodePropsPanel = nullptr;
    QDockWidget *m_nodeCanvasDock = nullptr;
    QDockWidget *m_nodePropsDock = nullptr;
    NodeEvaluator *m_nodeEvaluator = nullptr;
    bool m_nodeModeActive = false;
    QString m_nodeModeClipId;

    QAction *m_nodeModeAction = nullptr;

    void setupNodeCompositingDocks();
    void toggleNodeCompositingMode(bool on);
    void onNodeGraphChanged();
    void onNodeSelected(int id);
};
