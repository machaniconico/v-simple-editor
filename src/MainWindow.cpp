#include "MainWindow.h"
#include "VideoPlayer.h"
#include "Timeline.h"
#include "ExportDialog.h"
#include "UndoManager.h"
#include "OverlayDialogs.h"
#include "VideoEffectDialogs.h"
#include "EffectPlugin.h"
#include "ColorGradingPanel.h"
#include "EffectControlsPanel.h"
#include "LumetriScopes.h"
#include "EffectClipboard.h"
#include "PasteAttributesDialog.h"
#include <QDockWidget>
#include "GLPreview.h"
#include "EqualizerPanel.h"
#include "CompressorPanel.h"
#include "ReverbPanel.h"
#include "NoiseReductionPanel.h"
#include "TitlePresetDialog.h"
#include "VoiceOverDialog.h"
#include "MultiCamDialog.h"
#include "RenderQueueDialog.h"
#include "SceneDetector.h"
#include "MotionStabilizer.h"
#include "AdjustmentLayer.h"
#include "SpeedRampData.h"
#include "ProxyManager.h"
#include "ProxyProgressDialog.h"
#include "ProxyManagementDialog.h"
#include <QApplication>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QProgressDialog>
#include <QShortcut>
#include <QInputDialog>
#include <QCloseEvent>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QPointer>
#include <QUrl>
#include <QDebug>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QScrollArea>
#include <QFrame>
#include <QInputDialog>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDir>
#include <cmath>
#include <array>
#include "AudioMeterWidget.h"
#include "GradientStopBar.h"
#include "BrushAnimationDialog.h"
#include "Keyframe.h"
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QSettings>
#include <QSlider>
#include <QWidgetAction>
#include <QColorDialog>
#include <QFormLayout>
#include <QLabel>
#include <QStandardItemModel>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    qInfo() << "MainWindow::ctor begin";
    resize(1280, 720);

    m_supportedFormats = {
        "MP4 (*.mp4)", "MKV (*.mkv)", "MOV (*.mov)",
        "WebM (*.webm)", "FLV (*.flv)"
    };

    m_exporter = new Exporter(this);

    // Recent files setup (must be before setupMenuBar which uses m_recentFilesManager)
    setupRecentFiles();

    setupUI();
    qInfo() << "MainWindow::setupUI done";
    m_timeline->setMarkerManager(&m_markerManager);
    setupMenuBar();
    qInfo() << "MainWindow::setupMenuBar done";
    setupToolBar();
    updateEditActions();
    updateTitle();

    // Shortcut manager - load saved shortcuts
    ShortcutManager::instance().loadShortcuts();

    // Script engine. init() probes python3/python via QProcess with 3-second
    // timeouts each — on Windows when Python isn't installed, the
    // Microsoft Store python stub launches a Store dialog and the wait can
    // block much longer, freezing the splash screen and "preventing the
    // app from starting" from the user's perspective. Defer init to after
    // the event loop starts so startup never blocks on it.
    m_scriptEngine = new ScriptEngine(this);
    QTimer::singleShot(0, this, [this]() {
        if (m_scriptEngine) m_scriptEngine->init();
    });

    // Auto-save setup
    m_autoSave = new AutoSave(this);
    m_autoSave->setProjectData([this]() -> QString {
        ProjectData data;
        data.config = m_projectConfig;
        data.videoTracks = m_timeline->allVideoTracks();
        data.audioTracks = m_timeline->allAudioTracks();
        data.playheadPos = m_timeline->playheadPosition();
        data.markIn = m_timeline->markedIn();
        data.markOut = m_timeline->markedOut();
        data.brushAnimations = m_brushAnimationEntries;
        return ProjectFile::toJsonString(data);
    });
    connect(m_autoSave, &AutoSave::autoSaved, this, [this](const QString &path) {
        statusBar()->showMessage("Auto-saved: " + QFileInfo(path).fileName(), 3000);
    });
    connect(m_autoSave, &AutoSave::recoveryAvailable, this, [](const QStringList &files) {
        // Crash Recovery dialog suppressed per user preference — silently
        // clean up stale recovery files on startup so the prompt never
        // blocks the app opening.
        for (const QString &path : files)
            QFile::remove(path);
    });
    // Apply dark theme by default
    ThemeManager::instance().applyTheme(ThemeType::Dark, this);

    // Enable drag & drop
    setAcceptDrops(true);

    // Setup permanent status bar widgets
    setupStatusBarWidgets();
    updateStatusInfo();

    statusBar()->showMessage("準備完了 — ファイル > 新規プロジェクトから開始してください");

    connect(m_timeline, &Timeline::clipSelected, this, [this](int /*index*/) {
        updateEditActions();
    });
    // V3 sprint — preview drag handle の edit target を選択 clip に同期。
    // Timeline 側で (sourceTrack, sourceClipIndex) を直接運んでくれる
    // track-aware overload にスイッチ。playhead heuristic は
    // V1+V2 が同 clipIdx を共有すると常に V1 を選んでしまう
    // failure mode があったので削除。
    connect(m_timeline, &Timeline::clipSelectedOnTrack, this,
        [this](int trackIdx, int clipIdx) {
            if (m_player)
                m_player->setEditTargetByClip(trackIdx, clipIdx);
            syncBrushAnimationPreviewForClip(trackIdx, clipIdx);
        });
    connect(m_timeline->undoManager(), &UndoManager::stateChanged, this, [this]() {
        updateEditActions();
    });
    connect(m_timeline->undoManager(), &UndoManager::stateJumpRequested,
            m_timeline, &Timeline::restoreState);

    // Show welcome screen initially
    showWelcomeScreen();

    // Restore saved window state
    restoreWindowState();

    m_timeline->setAudioMixer(m_player->audioMixer());

    rebuildAudioMeters();

    qInfo() << "MainWindow::ctor end";
}

double MainWindow::currentPlayheadSeconds() const
{
    return m_timeline ? m_timeline->playheadPosition() : 0.0;
}

QString MainWindow::brushClipId(int trackIdx, int clipIdx)
{
    return QStringLiteral("%1:%2").arg(trackIdx).arg(clipIdx);
}

void MainWindow::setBrushAnimationEntries(const QVector<BrushAnimationEntry> &entries)
{
    for (auto it = m_liveBrushAnimations.cbegin(); it != m_liveBrushAnimations.cend(); ++it) {
        if (it.value())
            it.value()->deleteLater();
    }
    m_liveBrushAnimations.clear();
    m_brushAnimationEntries = entries;
}

void MainWindow::upsertBrushAnimationEntry(const BrushAnimationEntry &entry)
{
    for (auto &existing : m_brushAnimationEntries) {
        if (existing.clipId == entry.clipId) {
            existing = entry;
            return;
        }
    }
    m_brushAnimationEntries.append(entry);
}

BrushAnimation *MainWindow::materializeBrushAnimation(const QString &clipId)
{
    if (auto *live = m_liveBrushAnimations.value(clipId, nullptr))
        return live;

    for (const auto &entry : m_brushAnimationEntries) {
        if (entry.clipId != clipId)
            continue;
        auto *animation = new BrushAnimation(this);
        animation->fromJson(entry.brushData);
        m_liveBrushAnimations.insert(clipId, animation);
        return animation;
    }
    return nullptr;
}

void MainWindow::syncBrushAnimationPreviewForClip(int trackIdx, int clipIdx)
{
    if (!m_player || !m_player->glPreview())
        return;

    if (!m_timeline || trackIdx < 0 || clipIdx < 0) {
        m_player->glPreview()->clearBrushAnimation();
        return;
    }

    auto *track = m_timeline->videoTracks().value(trackIdx, nullptr);
    if (!track) {
        m_player->glPreview()->clearBrushAnimation();
        return;
    }

    const auto &clips = track->clips();
    if (clipIdx >= clips.size()) {
        m_player->glPreview()->clearBrushAnimation();
        return;
    }

    auto *animation = materializeBrushAnimation(brushClipId(trackIdx, clipIdx));
    if (!animation) {
        m_player->glPreview()->clearBrushAnimation();
        return;
    }

    const double progress = clips[clipIdx].keyframes.valueAt(
        QStringLiteral("brush_progress"), 0.0, 0.0);
    m_player->glPreview()->setBrushAnimation(animation);
    m_player->glPreview()->setBrushAnimationProgress(progress);
}

void MainWindow::setupUI()
{
    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_mainSplitter = new QSplitter(Qt::Vertical, this);

    m_player = new VideoPlayer(this);
    connect(m_player, &VideoPlayer::proxySettingsRequested,
            this, &MainWindow::openProxySettings);
    m_timeline = new Timeline(this);
    // US-INT-1: hand the Timeline to GLPreview so paintGL can compose any
    // adjustment layers covering the current timeline position.
    if (m_player->glPreview())
        m_player->glPreview()->setTimeline(m_timeline);

    // Welcome widget (shown when empty)
    m_welcomeWidget = new WelcomeWidget(this);
    connect(m_welcomeWidget, &WelcomeWidget::newProjectClicked, this, &MainWindow::newProject);
    connect(m_welcomeWidget, &WelcomeWidget::openFileClicked, this, &MainWindow::openFile);
    connect(m_welcomeWidget, &WelcomeWidget::openProjectClicked, this, &MainWindow::openProject);

    // Adobe-style tool property layout: wrap the player in a horizontal
    // splitter whose right-hand child is a QStackedWidget hosting per-tool
    // property panels (empty page by default, text-tool page when active).
    // The horizontal splitter replaces the raw VideoPlayer as the top half
    // of m_mainSplitter so the timeline is unaffected.
    m_previewSplitter = new QSplitter(Qt::Horizontal, this);
    m_previewSplitter->addWidget(m_player);
    setupToolPropertyPanel();
    m_previewSplitter->addWidget(m_toolPropertyStack);
    m_previewSplitter->setStretchFactor(0, 1);
    m_previewSplitter->setStretchFactor(1, 0);
    m_previewSplitter->setCollapsible(0, false);
    m_previewSplitter->setCollapsible(1, true);
    m_toolPropertyStack->hide(); // collapsed by default — no tool active

    m_mainSplitter->addWidget(m_previewSplitter);
    m_mainSplitter->addWidget(m_timeline);
    m_mainSplitter->setStretchFactor(0, 3);
    m_mainSplitter->setStretchFactor(1, 1);
    // Iteration 14 — both children non-collapsible. User report:
    // 「プレビューの最小表示サイズはそのままに拡大出来る幅を少し増やせる？
    // (今の仕様だと一定以上大きくした後一気に最大サイズになる)」. With
    // QSplitter's default collapsible behavior, dragging the divider past
    // the timeline's collapse threshold snapped the timeline to 0 and the
    // preview to full height — the "一気に最大サイズ" jump. Non-collapsible
    // children stop the divider at each child's natural minimum, so the
    // preview's smooth scaling range extends up to window_height -
    // timeline_min instead of clamping at the snap point.
    m_mainSplitter->setCollapsible(0, false);
    m_mainSplitter->setCollapsible(1, false);

    connect(m_player, &VideoPlayer::textRectRequested,
            this, &MainWindow::onTextRectRequested);
    connect(m_player, &VideoPlayer::textInlineCommitted,
            this, &MainWindow::onTextInlineCommitted);
    connect(m_player, &VideoPlayer::textOverlayEditCommitted,
            this, &MainWindow::onTextOverlayEditCommitted);
    // Existing-overlay drag: rewrite the rect in place and re-push the
    // overlay list so the preview re-renders with the new geometry.
    connect(m_player, &VideoPlayer::textOverlayRectChanged, this,
            [this](int idx, const QRectF &normalizedRect) {
                if (!m_timeline) return;
                if (!m_timeline->updateTextOverlayRect(idx,
                        normalizedRect.x(), normalizedRect.y(),
                        normalizedRect.width(), normalizedRect.height()))
                    return;
                const auto &clips = m_timeline->videoClips();
                if (!clips.isEmpty() && m_player) {
                    QVector<EnhancedTextOverlay> overlays;
                    const auto &mgr = clips[0].textManager;
                    for (int i = 0; i < mgr.count(); ++i)
                        overlays.append(mgr.overlay(i));
                    m_player->setTextOverlays(overlays);
                }
            });
    // US-T35 Video source transform drag/resize → persist to owning clip.
    connect(m_player, &VideoPlayer::videoSourceTransformChanged, this,
            [this](int trackIdx, int clipIdx, double scale, double dx, double dy) {
                if (!m_timeline) return;
                m_timeline->setClipVideoTransform(trackIdx, clipIdx, scale, dx, dy);
            });
    // Timeline text-strip edge drag → update right-panel spinboxes + preview
    connect(m_timeline, &Timeline::textOverlayTimeChanged, this,
            [this](int idx, double startTime, double endTime) {
                if (m_textToolStartSpin) {
                    QSignalBlocker b(m_textToolStartSpin);
                    m_textToolStartSpin->setValue(startTime);
                }
                if (m_textToolEndSpin) {
                    QSignalBlocker b(m_textToolEndSpin);
                    m_textToolEndSpin->setValue(qMax(0.1, endTime - startTime));
                }
                // Re-push the overlay list so the preview re-renders with
                // the updated time range.
                const auto &clips = m_timeline->videoClips();
                if (!clips.isEmpty() && m_player) {
                    QVector<EnhancedTextOverlay> overlays;
                    const auto &mgr = clips[0].textManager;
                    for (int i = 0; i < mgr.count(); ++i)
                        overlays.append(mgr.overlay(i));
                    m_player->setTextOverlays(overlays);
                }
                statusBar()->showMessage(QString("テキスト時間: %1 s → %2 s (%3 s)")
                    .arg(startTime, 0, 'f', 2)
                    .arg(endTime, 0, 'f', 2)
                    .arg(endTime - startTime, 0, 'f', 2));
                (void)idx;
            });

    // Right-click clip menu → existing dialogs. Timeline doesn't own them.
    connect(m_timeline, &Timeline::transitionDialogRequested,
            this, &MainWindow::addTransition);
    connect(m_timeline, &Timeline::videoEffectsDialogRequested,
            this, &MainWindow::videoEffects);
    connect(m_timeline, &Timeline::colorCorrectionRequested,
            this, &MainWindow::colorCorrection);
    connect(m_timeline, &Timeline::transitionShortened,
            this, [this](const QString &name, double askedSec, double effSec) {
                statusBar()->showMessage(
                    QString("ハンドル不足: %1 を %2s → %3s に短縮しました")
                        .arg(name)
                        .arg(askedSec, 0, 'f', 2)
                        .arg(effSec,   0, 'f', 2),
                    5000);
            });

    mainLayout->addWidget(m_welcomeWidget);
    mainLayout->addWidget(m_mainSplitter);
    setCentralWidget(centralWidget);

    connect(m_player, &VideoPlayer::positionChanged, this, [this](double seconds) {
        if (m_timeline) {
            m_timeline->setPlayheadPosition(seconds);
        }
        emit playheadSecondsChanged(seconds);
    });
    connect(m_timeline, &Timeline::scrubPositionChanged, this, [this](double seconds) {
        m_player->previewSeek(qRound(static_cast<double>(seconds) * 1000.0));
    });
    connect(m_timeline, &Timeline::positionChanged, this, [this](double seconds) {
        m_player->seek(qRound(static_cast<double>(seconds) * 1000.0));
    });
    // Multi-clip playback: forward Timeline's resolved schedule to VideoPlayer.
    // Apply proxy-path translation here so VideoPlayer stays unaware of proxies.
    connect(m_timeline, &Timeline::sequenceChanged, this, [this](const QVector<PlaybackEntry> &entries) {
        if (!m_player) return;
        QVector<PlaybackEntry> resolved = entries;
        auto &pm = ProxyManager::instance();
        for (auto &e : resolved)
            e.filePath = pm.getProxyPath(e.filePath);
        qInfo() << "MainWindow: forwarding sequenceChanged entries=" << resolved.size();
        m_player->setSequence(resolved);
        // US-INT-2 Phase A: gather per-entry speed ramps in lockstep with
        // setSequence. Key by (sourceTrack, sourceClipIndex) — positional
        // alignment between resolved[] and any single track's clips() is
        // unsafe (gaps + overlaps mean the indexes diverge). Missing or
        // out-of-range clips fall back to identity so the parallel array
        // size always matches resolved.
        QVector<speedramp::SpeedRamp> videoRamps;
        videoRamps.reserve(resolved.size());
        const auto &vTracks = m_timeline->videoTracks();
        for (const auto &e : resolved) {
            if (e.sourceTrack >= 0 && e.sourceTrack < vTracks.size()) {
                const auto &clips = vTracks[e.sourceTrack]->clips();
                if (e.sourceClipIndex >= 0
                    && e.sourceClipIndex < clips.size()) {
                    videoRamps.append(clips[e.sourceClipIndex].speedRamp);
                    continue;
                }
            }
            videoRamps.append(speedramp::SpeedRamp::identity());
        }
        m_player->setSpeedRamps(videoRamps);
    });
    // Audio-side schedule — feeds AudioMixer so every active entry across
    // A1..A16 is sum-mixed into a single output. Unlinked A clips and
    // overlapping tracks all sound simultaneously.
    connect(m_timeline, &Timeline::audioSequenceChanged, this, [this](const QVector<PlaybackEntry> &entries) {
        if (!m_player) return;
        QVector<PlaybackEntry> resolved = entries;
        auto &pm = ProxyManager::instance();
        for (auto &e : resolved)
            e.filePath = pm.getProxyPath(e.filePath);
        qInfo() << "MainWindow: forwarding audioSequenceChanged entries=" << resolved.size();
        m_player->setAudioSequence(resolved);
        // US-INT-2 Phase A: forward audio-side speed ramps (stored only for
        // now; AudioMixer Phase B will read them under m_controlMutex when
        // per-fragment atempo lands).
        QVector<speedramp::SpeedRamp> audioRamps;
        audioRamps.reserve(resolved.size());
        const auto &aTracks = m_timeline->audioTracks();
        for (const auto &e : resolved) {
            if (e.sourceTrack >= 0 && e.sourceTrack < aTracks.size()) {
                const auto &clips = aTracks[e.sourceTrack]->clips();
                if (e.sourceClipIndex >= 0
                    && e.sourceClipIndex < clips.size()) {
                    audioRamps.append(clips[e.sourceClipIndex].speedRamp);
                    continue;
                }
            }
            audioRamps.append(speedramp::SpeedRamp::identity());
        }
        if (auto *mix = m_player->audioMixer())
            mix->setSpeedRamps(audioRamps);
    });
    // Per-track solo state lives on the mixer (effective gain applied per
    // entry); audioSequenceChanged alone can't carry it because solo is
    // global state, not a per-clip flag. Forward it directly.
    connect(m_timeline, &Timeline::trackSoloChanged, this, [this](int trackIdx, bool solo) {
        if (!m_player) return;
        if (auto *mixer = m_player->audioMixer())
            mixer->setTrackSolo(trackIdx, solo);
    });

    // Proxy generation progress dialog: modeless window created lazily on
    // the first proxyStarted and reused for the rest of the session, so the
    // user has a stable place to monitor / abort the queue.
    auto &proxyMgr = ProxyManager::instance();
    connect(&proxyMgr, &ProxyManager::proxyStarted, this, [this](const QString &clipName) {
        if (!m_proxyDialog) {
            m_proxyDialog = new ProxyProgressDialog(this);
            m_proxyDialog->setAttribute(Qt::WA_DeleteOnClose, false);
            connect(m_proxyDialog, &ProxyProgressDialog::cancelRequested,
                    &ProxyManager::instance(), &ProxyManager::cancelGeneration);
        }
        m_proxyDialog->onProxyStarted(clipName);
    });
    connect(&proxyMgr, &ProxyManager::proxyProgress, this,
            [this](const QString &clipName, int percent) {
        if (m_proxyDialog) m_proxyDialog->onProxyProgress(clipName, percent);
    });
    connect(&proxyMgr, &ProxyManager::proxyFinished, this,
            [this](const QString &clipName, bool ok) {
        if (m_proxyDialog) m_proxyDialog->onProxyFinished(clipName, ok);
    });
    connect(&proxyMgr, &ProxyManager::proxyCancelled, this,
            [this](const QString &clipName) {
        if (m_proxyDialog) m_proxyDialog->onProxyCancelled(clipName);
    });

    // J/K/L keyboard shortcuts for playback
    auto *jKey = new QShortcut(QKeySequence(Qt::Key_J), this);
    connect(jKey, &QShortcut::activated, m_player, &VideoPlayer::speedDown);
    auto *kKey = new QShortcut(QKeySequence(Qt::Key_K), this);
    connect(kKey, &QShortcut::activated, m_player, &VideoPlayer::togglePlay);
    auto *lKey = new QShortcut(QKeySequence(Qt::Key_L), this);
    connect(lKey, &QShortcut::activated, m_player, &VideoPlayer::speedUp);

    // Ctrl+Wheel zoom
    connect(m_player, &VideoPlayer::playbackSpeedChanged, this, [this](double speed) {
        statusBar()->showMessage(QString("Speed: %1x").arg(speed, 0, 'f', 1));
    });

    m_player->setMinimumHeight(280); // lowered so the timeline can grow taller
    m_mainSplitter->setSizes({680, 320});

    // Iteration 15 — preview maximize toggle. The button lives in the
    // VideoPlayer's bottom-right; clicking it (or Esc when active) hides
    // the timeline so the preview can occupy the full main splitter.
    connect(m_player, &VideoPlayer::previewMaximizeChanged, this,
            [this](bool maximized) {
                if (m_timeline) m_timeline->setVisible(!maximized);
            });
    auto *escKey = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escKey, &QShortcut::activated, this, [this]() {
        // US-WIRE-3: Esc cancels motion tracking region picker
        if (m_player && m_player->isRegionPickerActive()) {
            m_player->exitRegionPickerMode();
            statusBar()->showMessage(QString());
            return;
        }
        if (m_player && m_player->isPreviewMaximized()) {
            m_player->setPreviewMaximized(false);
        }
    });

    // Restore master loudness normalizer settings on startup.
    if (auto *mixer = m_player ? m_player->audioMixer() : nullptr) {
        QSettings audioPrefs("VSimpleEditor", "Preferences");
        mixer->setNormalizerAmount(
            audioPrefs.value("audio/normalizerAmount", 0.0).toDouble());
        mixer->setNormalizerUniformity(
            audioPrefs.value("audio/normalizerUniformity", 0.5).toDouble());
    }
}

void MainWindow::setupMenuBar()
{
    // ファイル メニュー
    auto *fileMenu = menuBar()->addMenu("ファイル(&F)");

    auto *newAction = fileMenu->addAction("新規プロジェクト(&N)...");
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::newProject);

    auto *projectSettingsAction = fileMenu->addAction("プロジェクト設定(&T)...");
    connect(projectSettingsAction, &QAction::triggered, this, &MainWindow::editProjectSettings);

    auto *openAction = fileMenu->addAction("ファイルを開く(&O)...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);

    // 最近使ったファイル
    m_recentFilesMenu = new RecentFilesMenu(m_recentFilesManager, fileMenu);
    m_recentFilesMenu->setTitle("最近使ったファイル");
    fileMenu->addMenu(m_recentFilesMenu);
    connect(m_recentFilesMenu, &RecentFilesMenu::fileSelected, this, &MainWindow::openRecentFile);

    fileMenu->addSeparator();

    auto *openProjectAction = fileMenu->addAction("プロジェクトを開く(&P)...");
    openProjectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(openProjectAction, &QAction::triggered, this, &MainWindow::openProject);

    auto *saveAction = fileMenu->addAction("プロジェクトを保存(&S)");
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::saveProject);

    auto *saveAsAction = fileMenu->addAction("名前を付けて保存(&A)...");
    saveAsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveProjectAs);

    fileMenu->addSeparator();

    fileMenu->addSeparator();

    // Premiere Multicam / Resolve Multicam Sync (simplified) parity —
    // standalone dialog that builds a MultiCamProject EDL.
    auto *multiCamDialogAction = fileMenu->addAction("マルチカメラ...");
    connect(multiCamDialogAction, &QAction::triggered, this, &MainWindow::openMultiCamDialog);

    // Premiere Media Encoder / Resolve Deliver page parity — modeless
    // dialog that lists pending / running / completed export jobs.
    auto *renderQueueDialogAction = fileMenu->addAction("レンダーキュー...");
    connect(renderQueueDialogAction, &QAction::triggered, this, &MainWindow::openRenderQueueDialog);

    fileMenu->addSeparator();

    auto *exportAction = fileMenu->addAction("エクスポート(&E)...");
    exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportVideo);

    auto *remotionAction = fileMenu->addAction("Remotion形式でエクスポート(&R)...");
    connect(remotionAction, &QAction::triggered, this, &MainWindow::exportToRemotion);

    fileMenu->addSeparator();

    auto *prefsMenu = fileMenu->addMenu("環境設定(&S)");
    fileMenu->addSeparator();

    auto *quitAction = fileMenu->addAction("終了(&Q)");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    // 編集 メニュー
    auto *editMenu = menuBar()->addMenu("編集(&E)");

    m_undoAction = editMenu->addAction("元に戻す(&U)");
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undoAction);

    m_redoAction = editMenu->addAction("やり直す(&R)");
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::redoAction);

    editMenu->addSeparator();

    m_copyAction = editMenu->addAction("クリップをコピー(&C)");
    m_copyAction->setShortcut(QKeySequence::Copy);
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::copyClip);

    m_pasteAction = editMenu->addAction("クリップを貼り付け(&P)");
    m_pasteAction->setShortcut(QKeySequence::Paste);
    connect(m_pasteAction, &QAction::triggered, this, &MainWindow::pasteClip);

    editMenu->addSeparator();

    m_splitAction = editMenu->addAction("再生ヘッドで分割(&S)");
    m_splitAction->setShortcut(QKeySequence(Qt::Key_S));
    connect(m_splitAction, &QAction::triggered, this, &MainWindow::splitClip);

    m_deleteAction = editMenu->addAction("クリップを削除(&D)");
    m_deleteAction->setShortcut(QKeySequence::Delete);
    connect(m_deleteAction, &QAction::triggered, this, &MainWindow::deleteClip);

    m_rippleDeleteAction = editMenu->addAction("リップル削除");
    m_rippleDeleteAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    connect(m_rippleDeleteAction, &QAction::triggered, this, &MainWindow::rippleDelete);

    editMenu->addSeparator();

    m_copyEffectsAction = editMenu->addAction("Copy Effects");
    m_copyEffectsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    connect(m_copyEffectsAction, &QAction::triggered, this, &MainWindow::copyEffects);

    m_pasteEffectsAction = editMenu->addAction("Paste Effects");
    m_pasteEffectsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    connect(m_pasteEffectsAction, &QAction::triggered, this, &MainWindow::pasteEffects);

    m_pasteAttributesAction = editMenu->addAction("Paste Attributes...");
    connect(m_pasteAttributesAction, &QAction::triggered, this, &MainWindow::pasteAttributes);

    auto &clipBoard = effectctrl::EffectClipboard::instance();
    m_pasteEffectsAction->setEnabled(clipBoard.hasContent());
    m_pasteAttributesAction->setEnabled(clipBoard.hasContent());
    connect(&clipBoard, &effectctrl::EffectClipboard::contentChanged, this, [this]() {
        const bool has = effectctrl::EffectClipboard::instance().hasContent();
        if (m_pasteEffectsAction) m_pasteEffectsAction->setEnabled(has);
        if (m_pasteAttributesAction) m_pasteAttributesAction->setEnabled(has);
    });

    editMenu->addSeparator();

    auto *applyDefaultTransAct = editMenu->addAction("規定トランジションを適用(&D)");
    applyDefaultTransAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(applyDefaultTransAct, &QAction::triggered, this, &MainWindow::applyDefaultTransition);

    auto *editDefaultTransAct = editMenu->addAction("規定トランジション設定...");
    connect(editDefaultTransAct, &QAction::triggered, this, &MainWindow::editDefaultTransition);

    editMenu->addSeparator();

    m_snapAction = editMenu->addAction("スナップ切替(&N)");
    m_snapAction->setShortcut(QKeySequence(Qt::Key_N));
    m_snapAction->setCheckable(true);
    m_snapAction->setChecked(true);
    connect(m_snapAction, &QAction::triggered, this, &MainWindow::toggleSnap);

    editMenu->addSeparator();

    auto *speedAction = editMenu->addAction("再生速度を設定...");
    connect(speedAction, &QAction::triggered, this, &MainWindow::setClipSpeed);

    // Premiere "Speed / Duration" parity — applies a flat SpeedRamp
    // curve to the selected clip via Timeline.
    auto *speedRampDialogAction = editMenu->addAction("速度 / 持続時間...");
    speedRampDialogAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(speedRampDialogAction, &QAction::triggered, this, &MainWindow::openSpeedRampDialog);

    editMenu->addSeparator();

    // Streaming chi-squared histogram cut detector (SceneDetector).
    // Decodes a sample of frames from the active clip and, for each
    // detected cut, drops a Timeline marker.
    auto *sceneDetectAction = editMenu->addAction("シーン検出 (自動)...");
    connect(sceneDetectAction, &QAction::triggered, this, &MainWindow::openSceneDetector);

    // MotionStabilizer — analyses the active clip for camera shake and
    // either bakes counter-translation keyframes (when supported) or
    // reports the result to the status bar (deferred integration).
    auto *stabilizeAct = editMenu->addAction("スタビライズ (手ブレ補正)...");
    connect(stabilizeAct, &QAction::triggered, this, &MainWindow::runMotionStabilizer);

    editMenu->addSeparator();

    auto *shortcutAction = editMenu->addAction("キーボードショートカット(&K)...");
    shortcutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_K));
    connect(shortcutAction, &QAction::triggered, this, &MainWindow::editShortcuts);
    prefsMenu->addAction(shortcutAction);

    // 表示 メニュー
    auto *viewMenu = menuBar()->addMenu("表示(&V)");

    auto *zoomInAction = viewMenu->addAction("拡大(&I)");
    zoomInAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal));
    connect(zoomInAction, &QAction::triggered, this, &MainWindow::zoomIn);

    auto *zoomOutAction = viewMenu->addAction("縮小(&O)");
    zoomOutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    connect(zoomOutAction, &QAction::triggered, this, &MainWindow::zoomOut);

    // トラック メニュー
    auto *trackMenu = menuBar()->addMenu("トラック(&T)");

    auto *addVTrack = trackMenu->addAction("ビデオトラックを追加(&V)");
    connect(addVTrack, &QAction::triggered, this, &MainWindow::addVideoTrack);

    auto *addATrack = trackMenu->addAction("オーディオトラックを追加(&A)");
    connect(addATrack, &QAction::triggered, this, &MainWindow::addAudioTrack);

    // 挿入 メニュー
    auto *insertMenu = menuBar()->addMenu("挿入(&I)");

    auto *addTextAction = insertMenu->addAction("テキスト / テロップ追加(&T)...");
    addTextAction->setShortcut(QKeySequence(Qt::Key_T));
    connect(addTextAction, &QAction::triggered, this, &MainWindow::addTextOverlay);

    auto *manageTextAction = insertMenu->addAction("テキスト管理(&M)...");
    manageTextAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    connect(manageTextAction, &QAction::triggered, this, &MainWindow::manageTextOverlays);

    auto *importSubAction = insertMenu->addAction("字幕をインポート (SRT/VTT)...");
    connect(importSubAction, &QAction::triggered, this, &MainWindow::importSubtitles);

    auto *exportTextAction = insertMenu->addAction("テキストを書き出し (SRT / CSV)...");
    connect(exportTextAction, &QAction::triggered, this, &MainWindow::exportTextOverlays);

    auto *saveTemplateAction = insertMenu->addAction("テキストテンプレートを保存...");
    connect(saveTemplateAction, &QAction::triggered, this, &MainWindow::saveTextTemplate);

    auto *addBrushAnimAction = insertMenu->addAction("ブラシ / 書き起こしアニメ追加(&B)...");
    addBrushAnimAction->setShortcut(QKeySequence(Qt::Key_B));
    connect(addBrushAnimAction, &QAction::triggered, this, &MainWindow::addBrushAnimation);

    insertMenu->addSeparator();

    auto *addTransAction = insertMenu->addAction("トランジションを追加...");
    connect(addTransAction, &QAction::triggered, this, &MainWindow::addTransition);

    auto *addImageAction = insertMenu->addAction("画像 / 静止画を追加...");
    connect(addImageAction, &QAction::triggered, this, &MainWindow::addImageOverlay);

    auto *addPipAction = insertMenu->addAction("ピクチャー・イン・ピクチャー追加...");
    connect(addPipAction, &QAction::triggered, this, &MainWindow::addPip);

    insertMenu->addSeparator();

    // Premiere Essential Graphics / Resolve Fusion Titles parity.
    auto *titlePresetAction = insertMenu->addAction("タイトルプリセット...");
    titlePresetAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y));
    connect(titlePresetAction, &QAction::triggered, this, &MainWindow::openTitlePresetDialog);

    // Photoshop / Premiere "Adjustment Layer" — a special timeline clip
    // that carries no video content of its own but applies grading
    // parameters to every video frame underneath.
    auto *addAdjustmentAction = insertMenu->addAction("調整レイヤー");
    connect(addAdjustmentAction, &QAction::triggered, this, &MainWindow::addAdjustmentLayerCmd);

    // オーディオ メニュー
    auto *audioMenu = menuBar()->addMenu("オーディオ(&A)");

    auto *volumeAction = audioMenu->addAction("音量を設定...");
    connect(volumeAction, &QAction::triggered, this, &MainWindow::setClipVolume);

    auto *bgmAction = audioMenu->addAction("BGM / 音声ファイルを追加...");
    connect(bgmAction, &QAction::triggered, this, &MainWindow::addBgm);

    auto *voiceOverAction = audioMenu->addAction("Voice-over Record...");
    voiceOverAction->setShortcut(QKeySequence(Qt::Key_F12));
    connect(voiceOverAction, &QAction::triggered, this, &MainWindow::openVoiceOverDialog);

    audioMenu->addSeparator();

    auto *muteAction = audioMenu->addAction("ミュート切替 (A1)");
    muteAction->setShortcut(QKeySequence(Qt::Key_M));
    connect(muteAction, &QAction::triggered, this, &MainWindow::toggleMute);

    auto *soloAction = audioMenu->addAction("ソロ切替 (A1)");
    connect(soloAction, &QAction::triggered, this, &MainWindow::toggleSolo);

    audioMenu->addSeparator();

    auto *eqAction = audioMenu->addAction("イコライザー...");
    connect(eqAction, &QAction::triggered, this, &MainWindow::audioEqualizer);

    auto *audioFxAction = audioMenu->addAction("オーディオエフェクト...");
    connect(audioFxAction, &QAction::triggered, this, &MainWindow::audioEffects);

    audioMenu->addSeparator();

    auto *vstAction = audioMenu->addAction("VST / AUプラグイン...");
    connect(vstAction, &QAction::triggered, this, &MainWindow::openVSTPlugins);

    audioMenu->addSeparator();

    // Track EQ submenu — rebuilt on aboutToShow with one entry per
    // audio track, each exposing the 5 built-in EQ presets.
    auto *trackEqMenu = audioMenu->addMenu("Track EQ");
    connect(trackEqMenu, &QMenu::aboutToShow, this, [this, trackEqMenu]() {
        trackEqMenu->clear();
        if (!m_timeline) return;
        const int trackCount = m_timeline->audioTrackCount();
        if (trackCount == 0) {
            auto *info = trackEqMenu->addAction("オーディオトラックがありません");
            info->setEnabled(false);
            return;
        }
        const auto presets = AudioEQProcessor::presets();
        auto *mixer = m_player ? m_player->audioMixer() : nullptr;
        for (int i = 0; i < trackCount; ++i) {
            auto *trackSub = trackEqMenu->addMenu(QString("A%1").arg(i + 1));
            for (const auto &preset : presets) {
                auto *act = trackSub->addAction(preset.name);
                const int ti = i;
                const AudioEQConfig cfg = preset.config;
                const QString presetName = preset.name;
                connect(act, &QAction::triggered, this,
                        [this, mixer, ti, cfg, presetName]() {
                    if (mixer) {
                        mixer->setTrackEqEnabled(ti, true);
                        mixer->setTrackEqConfig(ti, cfg);
                    }
                    statusBar()->showMessage(
                        QString("A%1 EQ → %2").arg(ti + 1).arg(presetName),
                        3000);
                });
            }
            trackSub->addSeparator();
            auto *bypassAct = trackSub->addAction("Bypass / Reset");
            const int ti2 = i;
            connect(bypassAct, &QAction::triggered, this,
                    [this, mixer, ti2]() {
                if (mixer) {
                    mixer->setTrackEqEnabled(ti2, false);
                }
                statusBar()->showMessage(
                    QString("A%1 EQ bypassed").arg(ti2 + 1), 3000);
           });
        }
    });

    // Master Compressor dialog
    auto *compressorAction = audioMenu->addAction("Master Compressor...");
    connect(compressorAction, &QAction::triggered, this, &MainWindow::openMasterCompressor);

    audioMenu->addSeparator();

    // Pro-NLE "rubber band" volume envelope. When ON, every audio row
    // overlays its per-clip envelope; left-click adds a point, drag moves
    // it, right-click removes it. AudioMixer interpolates linearly.
    auto *envelopeAction = audioMenu->addAction("ボリュームエンベロープ編集モード");
    envelopeAction->setCheckable(true);
    // Ctrl+Shift+E so we don't shadow the existing File→Export Ctrl+E.
    envelopeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    connect(envelopeAction, &QAction::toggled, this,
            [this](bool on) {
                TimelineTrack::setEnvelopeEditMode(on);
                if (m_timeline) m_timeline->repaintAudioTracks();
                int trackCount = m_timeline ? m_timeline->audioTrackCount() : -1;
                int clipCount = 0;
                if (m_timeline) {
                    for (auto *t : m_timeline->audioTracks())
                        if (t) clipCount += t->clipCount();
                }
                statusBar()->showMessage(
                    QString("ボリュームエンベロープ %1 (audio tracks=%2, total clips=%3)")
                        .arg(on ? QStringLiteral("ON") : QStringLiteral("OFF"))
                        .arg(trackCount).arg(clipCount), 6000);
            });

    audioMenu->addSeparator();

    // Pro-NLE auto-ducking. Submenu rebuilt on aboutToShow so newly added
    // audio tracks appear without restart. Picking A<n> writes a -12 dB
    // envelope on every other audio track that overlaps that track's
    // clip ranges (200 ms attack / 400 ms release, Premiere defaults).
    auto *duckMenu = audioMenu->addMenu("BGM 自動ダッキング");
    connect(duckMenu, &QMenu::aboutToShow, this, [this, duckMenu]() {
        duckMenu->clear();
        if (!m_timeline) return;
        const int n = m_timeline->audioTrackCount();
        if (n < 2) {
            auto *info = duckMenu->addAction("オーディオトラックを 2 本以上に増やしてください");
            info->setEnabled(false);
            return;
        }
        for (int i = 0; i < n; ++i) {
            auto *act = duckMenu->addAction(
                QString("A%1 を voice 扱いして他トラックをダッキング").arg(i + 1));
            const int ti = i;
            connect(act, &QAction::triggered, this, [this, ti]() {
                if (!m_timeline) return;
                auto *mixer = m_player ? m_player->audioMixer() : nullptr;
                if (mixer) {
                    const auto &ad = mixer->autoDuckParams();
                    const double duckGain = std::pow(10.0, ad.thresholdDb / 20.0);
                    const double attackSec = ad.attackMs / 1000.0;
                    const double releaseSec = ad.releaseMs / 1000.0;
                    m_timeline->applyDuckingFromTrack(ti, duckGain, attackSec, releaseSec);
                } else {
                    m_timeline->applyDuckingFromTrack(ti);
                }
                statusBar()->showMessage(
                    QString("A%1 を voice 扱いして他オーディオトラックをダッキングしました").arg(ti + 1),
                    4000);
            });
        }
    });

    auto *duckSettingsAction = audioMenu->addAction("Auto-Duck Settings...");
    connect(duckSettingsAction, &QAction::triggered, this, &MainWindow::openAutoDuckSettings);

    audioMenu->addSeparator();

    // Per-track DSP panels (4-band EQ, compressor/limiter, Schroeder
    // reverb, noise reduction). Each opens / toggles a right-docked
    // QDockWidget that owns the panel widget; on change the panel signal
    // calls AudioMixer::setEqForTrack / setCompressorForTrack /
    // setReverbForTrack / setNoiseReductionForTrack.
    auto *eqPanelAction = audioMenu->addAction("EQ パネル...");
    connect(eqPanelAction, &QAction::triggered, this, &MainWindow::openEqualizerPanel);

    auto *compPanelAction = audioMenu->addAction("コンプレッサー / リミッター...");
    connect(compPanelAction, &QAction::triggered, this, &MainWindow::openCompressorPanel);

    auto *reverbPanelAction = audioMenu->addAction("リバーブ...");
    connect(reverbPanelAction, &QAction::triggered, this, &MainWindow::openReverbPanel);

    auto *nrPanelAction = audioMenu->addAction("ノイズリダクション...");
    connect(nrPanelAction, &QAction::triggered, this, &MainWindow::openNoiseReductionPanel);

    // マーカー メニュー
    auto *markersMenu = menuBar()->addMenu("マーカー(&K)");

    auto *addMarkerAction = markersMenu->addAction("再生ヘッドにマーカー追加");
    addMarkerAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    connect(addMarkerAction, &QAction::triggered, this, &MainWindow::addMarker);

    // Quick "M" key — Premiere/Resolve parity. Uses default red marker
    // colour and an empty label so the user gets a marker without a
    // dialog interrupt; rename via 全マーカーを表示...
    auto *quickMarkerAction = markersMenu->addAction("マーカー追加 (クイック)");
    quickMarkerAction->setShortcut(QKeySequence(Qt::Key_M));
    connect(quickMarkerAction, &QAction::triggered, this, &MainWindow::addQuickMarker);

    // Shift+M — open colour picker first, then drop a marker tagged with
    // the chosen colour. Persists colour into Timeline marker data via
    // Timeline::addMarker(timelineUs, label, color).
    auto *colouredMarkerAction = markersMenu->addAction("色付きマーカー追加...");
    colouredMarkerAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_M));
    connect(colouredMarkerAction, &QAction::triggered, this, &MainWindow::addColoredMarker);

    auto *nextMarkerAction = markersMenu->addAction("次のマーカーへジャンプ");
    nextMarkerAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right));
    connect(nextMarkerAction, &QAction::triggered, this, &MainWindow::jumpToNextMarker);

    auto *prevMarkerAction = markersMenu->addAction("前のマーカーへジャンプ");
    prevMarkerAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left));
    connect(prevMarkerAction, &QAction::triggered, this, &MainWindow::jumpToPrevMarker);

    auto *showMarkersAction = markersMenu->addAction("全マーカーを表示...");
    connect(showMarkersAction, &QAction::triggered, this, &MainWindow::showMarkers);

    auto *exportChapAction = markersMenu->addAction("YouTubeチャプターをエクスポート...");
    connect(exportChapAction, &QAction::triggered, this, &MainWindow::exportChapters);

    // エフェクト メニュー
    auto *effectsMenu = menuBar()->addMenu("エフェクト(&F)");

    auto *ccAction = effectsMenu->addAction("色補正 / グレーディング(&C)...");
    ccAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    connect(ccAction, &QAction::triggered, this, &MainWindow::colorCorrection);

    auto *fxAction = effectsMenu->addAction("ビデオエフェクト(&V)...");
    fxAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    connect(fxAction, &QAction::triggered, this, &MainWindow::videoEffects);

    effectsMenu->addSeparator();

    auto *sharpenFxAction = effectsMenu->addAction("シャープン...");
    connect(sharpenFxAction, &QAction::triggered, this, &MainWindow::applySharpenEffect);

    auto *mosaicFxAction = effectsMenu->addAction("モザイク...");
    connect(mosaicFxAction, &QAction::triggered, this, &MainWindow::applyMosaicEffect);

    auto *chromaFxAction = effectsMenu->addAction("クロマキー...");
    connect(chromaFxAction, &QAction::triggered, this, &MainWindow::applyChromaKeyEffect);

    effectsMenu->addSeparator();

    auto *pluginAction = effectsMenu->addAction("プラグインエフェクト(&P)...");
    connect(pluginAction, &QAction::triggered, this, &MainWindow::pluginEffects);

    effectsMenu->addSeparator();

    auto *lutAction = effectsMenu->addAction("LUT適用 (.cube)...");
    connect(lutAction, &QAction::triggered, this, &MainWindow::applyLut);

    auto *manageLutAction = effectsMenu->addAction("LUT管理...");
    connect(manageLutAction, &QAction::triggered, this, &MainWindow::manageLuts);

    effectsMenu->addSeparator();

    auto *loadLutCubeAction = effectsMenu->addAction("LUT を読み込み…");
    connect(loadLutCubeAction, &QAction::triggered, this, &MainWindow::loadLutCubeFile);

    m_lutIntensitySlider = new QSlider(Qt::Horizontal, this);
    m_lutIntensitySlider->setRange(0, 100);
    m_lutIntensitySlider->setValue(100);
    m_lutIntensitySlider->setToolTip("LUT Intensity (0-100%)");
    connect(m_lutIntensitySlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_player)
            m_player->glPreview()->setLutIntensity(value / 100.0);
    });
    auto *lutSliderAction = new QWidgetAction(this);
    lutSliderAction->setDefaultWidget(m_lutIntensitySlider);
    effectsMenu->addAction(lutSliderAction);

    auto *clearLutMenuAction = effectsMenu->addAction("LUT 解除");
    connect(clearLutMenuAction, &QAction::triggered, this, &MainWindow::clearLutIntensity);

    effectsMenu->addSeparator();

    auto *applyPresetAction = effectsMenu->addAction("エフェクトプリセット適用...");
    applyPresetAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    connect(applyPresetAction, &QAction::triggered, this, &MainWindow::applyEffectPreset);

    auto *savePresetAction = effectsMenu->addAction("現在設定をプリセットに保存...");
    connect(savePresetAction, &QAction::triggered, this, &MainWindow::saveEffectPreset);

    auto *managePresetsAction = effectsMenu->addAction("プリセット管理...");
    connect(managePresetsAction, &QAction::triggered, this, &MainWindow::manageEffectPresets);

    effectsMenu->addSeparator();

    auto *kfAction = effectsMenu->addAction("キーフレーム編集(&K)...");
    kfAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(kfAction, &QAction::triggered, this, &MainWindow::editKeyframes);

    effectsMenu->addSeparator();

    // US-FEAT-D: motion tracking UI
    m_trackMotionAction = effectsMenu->addAction("Track Motion…");
    m_trackMotionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    connect(m_trackMotionAction, &QAction::triggered, this, &MainWindow::trackMotion);

    effectsMenu->addSeparator();

    auto *shaderFxAction = effectsMenu->addAction("GPUシェーダーエフェクト...");
    connect(shaderFxAction, &QAction::triggered, this, &MainWindow::applyShaderEffect);

    auto *manageShaderAction = effectsMenu->addAction("GPUシェーダー管理...");
    connect(manageShaderAction, &QAction::triggered, this, &MainWindow::manageShaderEffects);

    // 再生 メニュー
    auto *playbackMenu = menuBar()->addMenu("再生(&P)");

    auto *jklNote = playbackMenu->addAction("J/K/L 速度コントロール");
    jklNote->setEnabled(false);

    playbackMenu->addSeparator();

    auto *markInAction = playbackMenu->addAction("イン点をマーク(&I)");
    markInAction->setShortcut(QKeySequence(Qt::Key_I));
    connect(markInAction, &QAction::triggered, this, &MainWindow::markIn);

    auto *markOutAction = playbackMenu->addAction("アウト点をマーク(&O)");
    markOutAction->setShortcut(QKeySequence(Qt::Key_O));
    connect(markOutAction, &QAction::triggered, this, &MainWindow::markOut);

    // ツール メニュー (AI / 自動編集)
    auto *toolsMenu = menuBar()->addMenu("ツール(&T)");

    auto *silenceAction = toolsMenu->addAction("無音検出...");
    connect(silenceAction, &QAction::triggered, this, &MainWindow::autoSilenceDetect);

    auto *jumpCutAction = toolsMenu->addAction("自動ジャンプカット...");
    connect(jumpCutAction, &QAction::triggered, this, &MainWindow::autoJumpCut);

    auto *sceneAction = toolsMenu->addAction("シーン変化検出...");
    connect(sceneAction, &QAction::triggered, this, &MainWindow::autoSceneDetect);

    toolsMenu->addSeparator();

    auto *stabilizeAction = toolsMenu->addAction("手ブレ補正...");
    connect(stabilizeAction, &QAction::triggered, this, &MainWindow::stabilizeVideo);

    auto *speedRampAction = toolsMenu->addAction("スピードランプ (可変速)...");
    connect(speedRampAction, &QAction::triggered, this, &MainWindow::setSpeedRamp);

    toolsMenu->addSeparator();

    auto *motionTrackAction = toolsMenu->addAction("モーショントラッキング...");
    connect(motionTrackAction, &QAction::triggered, this, &MainWindow::motionTrackSetup);

    toolsMenu->addSeparator();

    auto *audioDenoiseAction = toolsMenu->addAction("音声ノイズ除去...");
    connect(audioDenoiseAction, &QAction::triggered, this, &MainWindow::audioNoiseDenoise);

    auto *videoDenoiseAction = toolsMenu->addAction("映像ノイズ除去...");
    connect(videoDenoiseAction, &QAction::triggered, this, &MainWindow::videoNoiseDenoise);

    toolsMenu->addSeparator();

    auto *subtitleGenAction = toolsMenu->addAction("字幕自動生成 (Whisper)...");
    connect(subtitleGenAction, &QAction::triggered, this, &MainWindow::generateSubtitles);

    auto *highlightAction = toolsMenu->addAction("AI自動ハイライト...");
    connect(highlightAction, &QAction::triggered, this, &MainWindow::analyzeHighlights);

    toolsMenu->addSeparator();

    auto *screenRecAction = toolsMenu->addAction("画面録画を開始...");
    connect(screenRecAction, &QAction::triggered, this, &MainWindow::startScreenRecording);

    auto *stopRecAction = toolsMenu->addAction("画面録画を停止");
    connect(stopRecAction, &QAction::triggered, this, &MainWindow::stopScreenRecording);

    toolsMenu->addSeparator();

    auto *proxySettingsAction = toolsMenu->addAction("プロキシ設定...");
    connect(proxySettingsAction, &QAction::triggered, this, &MainWindow::openProxySettings);

    auto *proxyToggle = toolsMenu->addAction("プロキシモード切替");
    proxyToggle->setCheckable(true);
    connect(proxyToggle, &QAction::triggered, this, &MainWindow::toggleProxyMode);

    auto *genProxiesAction = toolsMenu->addAction("プロキシ生成...");
    connect(genProxiesAction, &QAction::triggered, this, &MainWindow::generateProxies);

    auto *proxyMgmtAction = toolsMenu->addAction("プロキシ管理...");
    connect(proxyMgmtAction, &QAction::triggered, this, &MainWindow::openProxyManagement);

    auto *renderQueueAction = toolsMenu->addAction("レンダーキュー...");
    renderQueueAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    connect(renderQueueAction, &QAction::triggered, this, &MainWindow::openRenderQueue);

    auto *networkRenderAction = toolsMenu->addAction("ネットワークレンダー...");
    connect(networkRenderAction, &QAction::triggered, this, &MainWindow::openNetworkRender);

    toolsMenu->addSeparator();

    auto *scriptAction = toolsMenu->addAction("Pythonスクリプトコンソール...");
    scriptAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(scriptAction, &QAction::triggered, this, &MainWindow::openScriptConsole);

    toolsMenu->addSeparator();

    auto *multiCamSetupAction = toolsMenu->addAction("マルチカメラセットアップ...");
    connect(multiCamSetupAction, &QAction::triggered, this, &MainWindow::multiCamSetup);

    auto *multiCamSwitchAction = toolsMenu->addAction("マルチカメラ切替...");
    connect(multiCamSwitchAction, &QAction::triggered, this, &MainWindow::multiCamSwitch);

    // コンポジション メニュー (After Effects風)
    auto *compMenu = menuBar()->addMenu("コンポジション(&C)");

    auto *addShapeAction = compMenu->addAction("シェイプレイヤー追加...");
    connect(addShapeAction, &QAction::triggered, this, &MainWindow::addShapeLayer);

    auto *addParticleAction = compMenu->addAction("パーティクルエフェクト追加...");
    connect(addParticleAction, &QAction::triggered, this, &MainWindow::addParticleEffect);

    auto *textAnimAction = compMenu->addAction("テキストアニメーション追加...");
    connect(textAnimAction, &QAction::triggered, this, &MainWindow::addTextAnimation);

    compMenu->addSeparator();

    auto *transformKfAction = compMenu->addAction("トランスフォームキーフレーム編集...");
    connect(transformKfAction, &QAction::triggered, this, &MainWindow::editTransformKeyframes);

    auto *maskAction = compMenu->addAction("マスク追加...");
    connect(maskAction, &QAction::triggered, this, &MainWindow::addMask);

    auto *warpAction = compMenu->addAction("ワープ / 歪みエフェクト...");
    connect(warpAction, &QAction::triggered, this, &MainWindow::applyWarpEffect);

    compMenu->addSeparator();

    auto *exprAction = compMenu->addAction("エクスプレッション...");
    connect(exprAction, &QAction::triggered, this, &MainWindow::editExpressions);

    auto *precompAction = compMenu->addAction("選択をプリコンポーズ...");
    connect(precompAction, &QAction::triggered, this, &MainWindow::precomposeSelected);

    // --- カラーグレーディングパネル ---
    m_colorGradingPanel = new ColorGradingPanel(this);
    m_colorGradingPanel->setVisible(false); // 作成直後に非表示設定
    addDockWidget(Qt::RightDockWidgetArea, m_colorGradingPanel);
    m_colorGradingPanel->setLutList(LutLibrary::instance().allLuts());
    m_colorGradingPanel->close(); // 初期非表示を確実にする

    connect(m_colorGradingPanel, &ColorGradingPanel::colorCorrectionChanged,
            this, [this](const ColorCorrection &cc) {
        if (m_timeline->hasSelection()) {
            m_timeline->setClipColorCorrection(cc);
            m_player->setColorCorrection(cc);
        }
    });
    connect(m_colorGradingPanel, &ColorGradingPanel::lutSelected,
            this, [this](const QString &name) {
        if (name.isEmpty()) {
            m_player->glPreview()->clearLut();
            return;
        }
        LutData lut = LutLibrary::instance().findByName(name);
        if (lut.isValid()) {
            lut.intensity = m_colorGradingPanel->lutIntensity();
            m_player->glPreview()->setLut(lut);
        }
    });
    connect(m_colorGradingPanel, &ColorGradingPanel::lutIntensityChanged,
            this, [this](double intensity) {
        QString name = m_colorGradingPanel->selectedLutName();
        if (name.isEmpty()) return;
        LutData lut = LutLibrary::instance().findByName(name);
        if (lut.isValid()) {
            lut.intensity = intensity;
            m_player->glPreview()->setLut(lut);
        }
    });
    connect(m_colorGradingPanel, &ColorGradingPanel::resetRequested,
            this, [this]() {
        if (m_timeline->hasSelection()) {
            ColorCorrection cc;
            m_timeline->setClipColorCorrection(cc);
            m_player->setColorCorrection(cc);
            m_player->glPreview()->clearLut();
        }
    });
    // US-WIRE-2: wire ColorGradingPanel wheels → GLPreview shader
    connect(m_colorGradingPanel, &ColorGradingPanel::colorWheelsChanged,
            this, [this](const ColorWheels &cw) {
        if (!m_player || !m_player->glPreview())
            return;
        std::array<std::array<double,4>,3> values;
        values[0] = {static_cast<double>(cw.lift.x()),
                     static_cast<double>(cw.lift.y()),
                     static_cast<double>(cw.lift.z()),
                     cw.liftLuma};
        values[1] = {static_cast<double>(cw.gamma.x()),
                     static_cast<double>(cw.gamma.y()),
                     static_cast<double>(cw.gamma.z()),
                     cw.gammaLuma};
        values[2] = {static_cast<double>(cw.gain.x()),
                     static_cast<double>(cw.gain.y()),
                     static_cast<double>(cw.gain.z()),
                     cw.gainLuma};
        m_player->glPreview()->setLiftGammaGain(values);
    });

    // US-CG-1: wire ColorGradingPanel RGB Curves → GLPreview shader.
    // ColorGradingPanel re-emits CurveEditor::curvesChanged here.
    connect(m_colorGradingPanel, &ColorGradingPanel::curvesChanged,
            this, [this](const QVector<QVector<int>> &curves) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setRgbCurves(curves);
    });

    // US-CG-2: wire ColorGradingPanel White-Balance sliders → GLPreview uWb.
    // Sits at the very top of the grade chain (BEFORE LGG / curves / LUT).
    connect(m_colorGradingPanel, &ColorGradingPanel::whiteBalanceChanged,
            this, [this](float r, float g, float b) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setWhiteBalance(r, g, b);
    });

    // US-CG-3: wire ColorGradingPanel Vignette sliders → GLPreview uVig*.
    // Applied AFTER curves (US-CG-1) and BEFORE the .cube LUT (US-WIRE-1).
    connect(m_colorGradingPanel, &ColorGradingPanel::vignetteChanged,
            this, [this](float amount, float midpoint, float roundness, float feather) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setVignette(amount, midpoint, roundness, feather);
    });

    // US-CG-4: wire ColorGradingPanel Hue vs Saturation curve →
    // GLPreview::setHueVsSatLut. ColorGradingPanel re-emits the embedded
    // HueVsSatEditor's hueVsSatChanged signal here.
    connect(m_colorGradingPanel, &ColorGradingPanel::hueVsSatChanged,
            this, [this](const QVector<float> &lut) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setHueVsSatLut(lut);
    });

    // US-EF-1: wire ColorGradingPanel Chroma Key controls → GLPreview
    // uChroma* uniforms. Applied at the very TOP of the compose path (BEFORE
    // WB / LGG / curves / vignette / LUT) so the HSL gating + spill suppress
    // operate on raw frame colour. enabled=false is a free no-op.
    connect(m_colorGradingPanel, &ColorGradingPanel::chromaKeyChanged,
            this, [this](bool enabled, float keyH, float keyS, float keyL,
                         float hueTol, float satTol, float lumTol,
                         float spill, float softness) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setChromaKey(enabled, keyH, keyS, keyL,
                                            hueTol, satTol, lumTol,
                                            spill, softness);
    });

    // US-EF-2: wire ColorGradingPanel Mask controls → GLPreview uMask*
    // uniforms. The mask wraps the entire grade chain so the colour grade
    // applies INSIDE the mask region (or outside when invert=true).
    // enabled=false is a free no-op.
    connect(m_colorGradingPanel, &ColorGradingPanel::maskChanged,
            this, [this](bool enabled, bool ellipse, bool invert, float feather,
                         QRectF rect) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setMask(enabled, ellipse, invert, feather, rect);
    });

    // US-EF-3: wire ColorGradingPanel HSL Qualifier controls → GLPreview
    // uHslq* uniforms. The qualifier sits AFTER chroma key and BEFORE WB so
    // it operates on raw frame colour and applies a SECONDARY lift/gamma/
    // gain only inside the qualified hue/sat/luma region. enabled=false is
    // a free no-op (entire shader stage skipped).
    connect(m_colorGradingPanel, &ColorGradingPanel::hslQualifierChanged,
            this, [this](bool enabled,
                         float hueCenter, float hueRange,
                         float satMin, float satMax,
                         float lumaMin, float lumaMax,
                         float softness,
                         float liftR, float liftG, float liftB,
                         float gammaR, float gammaG, float gammaB,
                         float gainR, float gainG, float gainB) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setHslQualifier(enabled,
                                               hueCenter, hueRange,
                                               satMin, satMax,
                                               lumaMin, lumaMax,
                                               softness,
                                               liftR, liftG, liftB,
                                               gammaR, gammaG, gammaB,
                                               gainR, gainG, gainB);
    });

    // US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens
    // Distortion. ColorGradingPanel emits the raw slider scalars; GLPreview
    // applies the per-stage multipliers (×0.01 / px / ×0.01) inside the
    // fragment shader. Identity (0, 0, 0) is a free no-op (the shader's
    // |amount|>eps tests skip each kernel/transform entirely).
    connect(m_colorGradingPanel, &ColorGradingPanel::effectsPackChanged,
            this, [this](float sharpen, float blur, float lens) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setSharpen(sharpen);
        m_player->glPreview()->setBlur(blur);
        m_player->glPreview()->setLensDistortion(lens);
    });

    // US-EFC-1: Effect Controls panel — per-clip effect parameter panel.
    m_effectControlsPanel = new effectctrl::EffectControlsPanel(this);
    m_effectControlsPanel->setVisible(false);
    addDockWidget(Qt::RightDockWidgetArea, m_effectControlsPanel);
    m_effectControlsPanel->close();

    m_effectControlsPanel->setTimeline(m_timeline);
    m_effectControlsPanel->setMainWindow(this);
    connect(m_timeline, &Timeline::clipSelectedOnTrack,
            m_effectControlsPanel, &effectctrl::EffectControlsPanel::refreshFromCurrentClip);
    connect(m_effectControlsPanel, &effectctrl::EffectControlsPanel::effectsChanged,
            this, [this](const QVector<VideoEffect> &effects) {
        m_timeline->setClipEffects(effects);
    });

    // US-3D: 3-axis rotation + perspective foreshortening (Premiere "Basic
    // 3D" / Resolve "Transform" 3D rotation parity). Forwarded directly to
    // GLPreview::setRotation3D, which builds the 3x3 rotation matrix on the
    // CPU (intrinsic Tait-Bryan XYZ) and ships it to the fragment shader.
    // Identity (0,0,0,2.0) is a free no-op — the shader detects identity
    // and skips the warp entirely.
    connect(m_colorGradingPanel, &ColorGradingPanel::rotation3DChanged,
            this, [this](float xDeg, float yDeg, float zDeg, float persDist) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setRotation3D(xDeg, yDeg, zDeg, persDist);
    });

    // US-EF-2: "マスクを描画" → enter the mask drawing overlay on the
    // VideoPlayer. The callback feeds the normalized QRectF back to the
    // panel via setMaskRect, which re-emits maskChanged so GLPreview picks
    // up the new geometry. Reuses the US-WIRE-3 region picker overlay.
    connect(m_colorGradingPanel, &ColorGradingPanel::requestMaskDraw,
            this, [this]() {
        if (!m_player || !m_colorGradingPanel)
            return;
        ColorGradingPanel *panel = m_colorGradingPanel;
        m_player->enterMaskEditMode([panel](QRectF normalizedRect) {
            if (!panel) return;
            // QRectF() (Esc / aborted drag) is silently ignored.
            if (!normalizedRect.isValid()
                || normalizedRect.width() <= 0.0
                || normalizedRect.height() <= 0.0)
                return;
            panel->setMaskRect(normalizedRect);
        });
    });

    viewMenu->addSeparator();
    auto *colorPanelAction = viewMenu->addAction("カラーグレーディングパネル(&G)");
    colorPanelAction->setCheckable(true);
    colorPanelAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    connect(colorPanelAction, &QAction::toggled, m_colorGradingPanel, &QDockWidget::setVisible);
    connect(m_colorGradingPanel, &QDockWidget::visibilityChanged, colorPanelAction, &QAction::setChecked);

    auto *effectControlsAction = viewMenu->addAction("Effect Controls");
    effectControlsAction->setCheckable(true);
    connect(effectControlsAction, &QAction::toggled, m_effectControlsPanel, &QDockWidget::setVisible);
    connect(m_effectControlsPanel, &QDockWidget::visibilityChanged, effectControlsAction, &QAction::setChecked);

    // Lumetri Scopes dock — Histogram + Luma Waveform + Vectorscope. Off
    // by default so first-run users aren't paying CPU on scope math; the
    // toggle action lives next to the colour grading panel for discovery.
    auto *scopesDock = new QDockWidget("Lumetri Scopes", this);
    auto *scopes = new LumetriScopes(scopesDock);
    scopesDock->setWidget(scopes);
    scopesDock->setObjectName("LumetriScopesDock");
    addDockWidget(Qt::RightDockWidgetArea, scopesDock);
    scopesDock->setVisible(false);
    if (m_player)
        connect(m_player, &VideoPlayer::frameComposited,
                scopes, &LumetriScopes::setFrame);
    auto *scopesAction = viewMenu->addAction("Lumetri Scopes(&L)");
    scopesAction->setCheckable(true);
    scopesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    connect(scopesAction, &QAction::toggled, scopesDock, &QDockWidget::setVisible);
    connect(scopesDock, &QDockWidget::visibilityChanged, scopesAction, &QAction::setChecked);

    // Audio Meters dock
    m_audioMetersDock = new QDockWidget("Audio Meters", this);
    m_audioMetersDock->setObjectName("AudioMetersDock");
    addDockWidget(Qt::RightDockWidgetArea, m_audioMetersDock);
    m_audioMetersDock->setVisible(true);
    auto *audioMetersAction = viewMenu->addAction("Audio Meters");
    audioMetersAction->setCheckable(true);
    audioMetersAction->setChecked(true);
    connect(audioMetersAction, &QAction::toggled, m_audioMetersDock, &QDockWidget::setVisible);
    connect(m_audioMetersDock, &QDockWidget::visibilityChanged, audioMetersAction, &QAction::setChecked);

    // History dock
    m_historyDock = new HistoryDockWidget(m_timeline->undoManager(), this);
    addDockWidget(Qt::RightDockWidgetArea, m_historyDock);
    m_historyDock->setVisible(true);
    auto *historyAction = viewMenu->addAction("History");
    historyAction->setCheckable(true);
    historyAction->setChecked(true);
    connect(historyAction, &QAction::toggled, m_historyDock, &QDockWidget::setVisible);
    connect(m_historyDock, &QDockWidget::visibilityChanged, historyAction, &QAction::setChecked);

    // 表示メニュー追加項目
    auto *themeAction = viewMenu->addAction("テーマ変更...");
    connect(themeAction, &QAction::triggered, this, &MainWindow::changeTheme);

    viewMenu->addSeparator();

    auto *tooltipAction = viewMenu->addAction("ツールバーのツールチップを表示");
    tooltipAction->setCheckable(true);
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        tooltipAction->setChecked(prefSettings.value("showTooltips", true).toBool());
    }
    connect(tooltipAction, &QAction::toggled, this, [this](bool checked) {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        prefSettings.setValue("showTooltips", checked);
        // Re-apply tooltips on all toolbar actions
        auto *toolbar = findChild<QToolBar *>("Main");
        if (!toolbar) return;
        if (checked) {
            statusBar()->showMessage("ツールチップ有効 — 再起動で反映");
        } else {
            for (auto *action : toolbar->actions())
                action->setToolTip("");
            statusBar()->showMessage("ツールバーのツールチップを無効化");
        }
    });

    auto *toolbarStyleAction = viewMenu->addAction("ツールバーをアイコンのみ表示");
    toolbarStyleAction->setCheckable(true);
    connect(toolbarStyleAction, &QAction::toggled, this, [this](bool iconOnly) {
        auto *toolbar = findChild<QToolBar *>();
        if (toolbar) {
            toolbar->setToolButtonStyle(iconOnly ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
            QSettings prefSettings("VSimpleEditor", "Preferences");
            prefSettings.setValue("toolbarIconOnly", iconOnly);
        }
    });

    // 取り込み配置ポリシー: 並列トラック (V2/A2...) か 現在トラック追加 (V1/A1 連結)
    auto *importPlacementGroup = new QActionGroup(this);
    importPlacementGroup->setExclusive(true);
    auto *importParallelAction = new QAction("取り込み：新しいトラックに並列配置", this);
    importParallelAction->setCheckable(true);
    importParallelAction->setActionGroup(importPlacementGroup);
    auto *importAppendAction = new QAction("取り込み：現在のトラックに追加", this);
    importAppendAction->setCheckable(true);
    importAppendAction->setActionGroup(importPlacementGroup);
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        const int saved = prefSettings.value("importPlacement",
                                              static_cast<int>(ImportPlacement::ParallelTrack)).toInt();
        if (saved == static_cast<int>(ImportPlacement::AppendToFirstTrack))
            importAppendAction->setChecked(true);
        else
            importParallelAction->setChecked(true);
    }
    connect(importParallelAction, &QAction::toggled, this, [this](bool checked) {
        if (!checked) return;
        QSettings prefSettings("VSimpleEditor", "Preferences");
        prefSettings.setValue("importPlacement", static_cast<int>(ImportPlacement::ParallelTrack));
        statusBar()->showMessage("取り込み先を V2/A2 並列配置に設定");
    });
    connect(importAppendAction, &QAction::toggled, this, [this](bool checked) {
        if (!checked) return;
        QSettings prefSettings("VSimpleEditor", "Preferences");
        prefSettings.setValue("importPlacement", static_cast<int>(ImportPlacement::AppendToFirstTrack));
        statusBar()->showMessage("取り込み先を V1/A1 追加に設定");
    });

    // 自動プロキシ生成: 重い素材 (AV1 / QHD+) 取り込み時の挙動 3 択
    auto *autoProxyGroup = new QActionGroup(this);
    autoProxyGroup->setExclusive(true);
    auto *autoProxyDisabledAction = new QAction("自動プロキシ生成: しない", this);
    autoProxyDisabledAction->setCheckable(true);
    autoProxyDisabledAction->setActionGroup(autoProxyGroup);
    auto *autoProxyMultiAction = new QAction("自動プロキシ生成: V2 以降のみ", this);
    autoProxyMultiAction->setCheckable(true);
    autoProxyMultiAction->setActionGroup(autoProxyGroup);
    auto *autoProxyAlwaysAction = new QAction("自動プロキシ生成: 常時", this);
    autoProxyAlwaysAction->setCheckable(true);
    autoProxyAlwaysAction->setActionGroup(autoProxyGroup);
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        const int saved = prefSettings.value("autoProxyMode",
                                              static_cast<int>(AutoProxyMode::MultiTrackOnly)).toInt();
        if (saved == static_cast<int>(AutoProxyMode::Disabled))
            autoProxyDisabledAction->setChecked(true);
        else if (saved == static_cast<int>(AutoProxyMode::Always))
            autoProxyAlwaysAction->setChecked(true);
        else
            autoProxyMultiAction->setChecked(true);
    }
    connect(autoProxyDisabledAction, &QAction::toggled, this, [this](bool checked) {
        if (!checked) return;
        QSettings("VSimpleEditor", "Preferences").setValue("autoProxyMode",
            static_cast<int>(AutoProxyMode::Disabled));
        statusBar()->showMessage("自動プロキシ生成を無効化");
    });
    connect(autoProxyMultiAction, &QAction::toggled, this, [this](bool checked) {
        if (!checked) return;
        QSettings("VSimpleEditor", "Preferences").setValue("autoProxyMode",
            static_cast<int>(AutoProxyMode::MultiTrackOnly));
        statusBar()->showMessage("自動プロキシ生成: V2 以降のみ");
    });
    connect(autoProxyAlwaysAction, &QAction::toggled, this, [this](bool checked) {
        if (!checked) return;
        QSettings("VSimpleEditor", "Preferences").setValue("autoProxyMode",
            static_cast<int>(AutoProxyMode::Always));
        statusBar()->showMessage("自動プロキシ生成: 常時");
    });

    // 自動保存（バックアップ）トグル — デフォルトOFF、30分周期
    auto *autoSaveAction = new QAction("自動保存を有効化 (30分ごと)", this);
    autoSaveAction->setCheckable(true);
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        autoSaveAction->setChecked(prefSettings.value("autoSaveEnabled", false).toBool());
    }
    connect(autoSaveAction, &QAction::toggled, this, [this](bool checked) {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        prefSettings.setValue("autoSaveEnabled", checked);
        if (!m_autoSave)
            return;
        if (checked) {
            AutoSaveConfig cfg;
            cfg.enabled = true;
            cfg.interval = prefSettings.value("autoSaveIntervalSec", 1800).toInt();
            m_autoSave->start(cfg);
            statusBar()->showMessage(QString("自動保存 ON (%1分ごと)").arg(cfg.interval / 60));
        } else {
            m_autoSave->stop();
            statusBar()->showMessage("自動保存 OFF");
        }
    });

    // 環境設定サブメニューに共有QActionを集約
    prefsMenu->addSeparator();
    prefsMenu->addAction(themeAction);
    prefsMenu->addAction(tooltipAction);
    prefsMenu->addAction(toolbarStyleAction);
    prefsMenu->addSeparator();
    prefsMenu->addAction(importParallelAction);
    prefsMenu->addAction(importAppendAction);
    prefsMenu->addSeparator();
    prefsMenu->addAction(autoProxyDisabledAction);
    prefsMenu->addAction(autoProxyMultiAction);
    prefsMenu->addAction(autoProxyAlwaysAction);
    prefsMenu->addSeparator();

    auto *gpuEffectsAction = new QAction("GPUエフェクトを使用", this);
    gpuEffectsAction->setCheckable(true);
    {
        QSettings gpuFxSettings("VSimpleEditor", "Preferences");
        gpuEffectsAction->setChecked(gpuFxSettings.value("gpuEffectsEnabled", true).toBool());
    }
    connect(gpuEffectsAction, &QAction::toggled, this, [this](bool on) {
        QSettings("VSimpleEditor", "Preferences").setValue("gpuEffectsEnabled", on);
        if (m_player && m_timeline && m_timeline->hasSelection())
            m_player->setPreviewEffects(m_timeline->clipEffects(), /*live=*/true);
        else if (m_player)
            m_player->setPreviewEffects({}, /*live=*/true);
    });
    prefsMenu->addAction(gpuEffectsAction);
    prefsMenu->addSeparator();

    // Iteration 12: toggle for auto-play on first clip drop. Default OFF
    // per user request — the auto-play side effect of Iteration 10
    // setSequence empty -> non-empty handler is now opt-in. VideoPlayer
    // reads QSettings("VSimpleEditor", "Preferences")/autoPlayOnFirstSequence
    // every setSequence call so the toggle takes effect immediately
    // without a restart.
    auto *autoPlayAction = new QAction("クリップ追加で自動再生", this);
    autoPlayAction->setCheckable(true);
    {
        QSettings autoPlaySettings("VSimpleEditor", "Preferences");
        autoPlayAction->setChecked(
            autoPlaySettings.value("autoPlayOnFirstSequence", false).toBool());
    }
    connect(autoPlayAction, &QAction::toggled, this, [](bool on) {
        QSettings("VSimpleEditor", "Preferences")
            .setValue("autoPlayOnFirstSequence", on);
    });
    prefsMenu->addAction(autoPlayAction);
    prefsMenu->addSeparator();

    auto *loudnessAction = new QAction(
        QStringLiteral("オーディオ均一化..."), this);
    loudnessAction->setStatusTip(QStringLiteral(
        "全トラックの出力レベルを動的に均一化 (FCP の Loudness 風)"));
    connect(loudnessAction, &QAction::triggered,
            this, &MainWindow::openLoudnessSettings);
    prefsMenu->addAction(loudnessAction);
    prefsMenu->addSeparator();

    // US-T39 Snap strength submenu — pulls/flushes the video source onto
    // the 16:9 canvas edges when dragging. Persisted via QSettings so the
    // user's preference survives restart.
    auto *snapMenu = prefsMenu->addMenu("画面フィット強度");
    auto *snapGroup = new QActionGroup(this);
    snapGroup->setExclusive(true);
    struct SnapPreset { const char *label; double px; };
    const SnapPreset snapPresets[] = {
        {"オフ",  0.0},
        {"弱",   6.0},
        {"中",  12.0},
        {"強",  24.0},
        {"最強", 48.0},
    };
    double savedSnap = 12.0;
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        savedSnap = prefSettings.value("snapStrength", 12.0).toDouble();
    }
    if (m_player)
        m_player->setSnapStrength(savedSnap);
    for (const auto &preset : snapPresets) {
        auto *act = new QAction(preset.label, this);
        act->setCheckable(true);
        act->setActionGroup(snapGroup);
        if (qFuzzyCompare(preset.px, savedSnap)
            || (preset.px == 0.0 && savedSnap <= 0.0))
            act->setChecked(true);
        const double px = preset.px;
        connect(act, &QAction::toggled, this, [this, px](bool checked) {
            if (!checked) return;
            if (m_player) m_player->setSnapStrength(px);
            QSettings prefSettings("VSimpleEditor", "Preferences");
            prefSettings.setValue("snapStrength", px);
            statusBar()->showMessage(
                QString("画面フィット強度: %1 px").arg(px == 0.0 ? QStringLiteral("オフ") : QString::number(px)));
        });
        snapMenu->addAction(act);
    }

    prefsMenu->addAction(autoSaveAction);

    // ヘルプ メニュー
    auto *helpMenu = menuBar()->addMenu("ヘルプ(&H)");

    auto *resourceGuideAction = helpMenu->addAction("無料素材ガイド...");
    resourceGuideAction->setShortcut(QKeySequence(Qt::Key_F1));
    connect(resourceGuideAction, &QAction::triggered, this, &MainWindow::showResourceGuide);

    helpMenu->addSeparator();

    auto *aboutAction = helpMenu->addAction("バージョン情報(&A)");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::about);
}

void MainWindow::setupToolBar()
{
    auto *toolbar = addToolBar("Main");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(20, 20));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto icon = [](const QString &name) { return QIcon(":/icons/" + name + ".svg"); };

    auto addBtn = [&](const QString &iconName, const QString &label,
                      const QString &tooltip, auto slot) {
        auto *action = toolbar->addAction(icon(iconName), label, this, slot);
        action->setToolTip(tooltip);
        return action;
    };

    addBtn("new", "新規", "新規プロジェクト (Ctrl+N)", &MainWindow::newProject);
    addBtn("open", "開く", "ファイルを開く (Ctrl+O)", &MainWindow::openFile);
    addBtn("save", "保存", "プロジェクトを保存 (Ctrl+S)", &MainWindow::saveProject);
    toolbar->addSeparator();
    addBtn("undo", "元に戻す", "元に戻す (Ctrl+Z)", &MainWindow::undoAction);
    addBtn("redo", "やり直し", "やり直し (Ctrl+Shift+Z)", &MainWindow::redoAction);
    toolbar->addSeparator();
    addBtn("split", "分割", "再生ヘッドで分割 (S)", &MainWindow::splitClip);
    addBtn("delete", "削除", "クリップ削除 (Del)", &MainWindow::deleteClip);
    addBtn("copy", "コピー", "クリップをコピー (Ctrl+C)", &MainWindow::copyClip);
    addBtn("paste", "貼付", "クリップを貼り付け (Ctrl+V)", &MainWindow::pasteClip);
    toolbar->addSeparator();
    // Text tool: toolbar button toggles Adobe-style text-tool mode instead
    // of opening the modal dialog directly. The legacy dialog is still
    // reachable via 挿入 → テキスト / テロップ追加 for users who prefer it.
    m_textToolAction = toolbar->addAction(icon("text"), "テキスト");
    m_textToolAction->setToolTip("テキストツール (T) — ドラッグでテキスト枠を指定");
    m_textToolAction->setCheckable(true);
    connect(m_textToolAction, &QAction::toggled, this, &MainWindow::onTextToolToggled);
    addBtn("color", "色補正", "色補正 (Ctrl+G)", &MainWindow::colorCorrection);
    addBtn("effects", "効果", "ビデオエフェクト (Ctrl+Shift+F)", &MainWindow::videoEffects);
    addBtn("marker", "マーカー", "マーカー追加 (Ctrl+M)", &MainWindow::addMarker);
    toolbar->addSeparator();
    addBtn("export", "出力", "動画をエクスポート (Ctrl+E)", &MainWindow::exportVideo);
    addBtn("record", "録画", "画面録画を開始", &MainWindow::startScreenRecording);

    // Apply saved tooltip preference
    QSettings settings("VSimpleEditor", "Preferences");
    bool showTooltips = settings.value("showTooltips", true).toBool();
    if (!showTooltips) {
        for (auto *action : toolbar->actions())
            action->setToolTip("");
    }
}

void MainWindow::updateEditActions()
{
    bool hasSel = m_timeline->hasSelection();
    m_deleteAction->setEnabled(hasSel);
    m_rippleDeleteAction->setEnabled(hasSel);
    m_copyAction->setEnabled(hasSel);
    m_pasteAction->setEnabled(m_timeline->hasClipboard());
    m_undoAction->setEnabled(m_timeline->canUndo());
    m_redoAction->setEnabled(m_timeline->canRedo());
}

void MainWindow::updateTitle()
{
    QString title = QString("V Simple Editor - %1 (%2 %3fps)")
        .arg(m_projectConfig.name)
        .arg(m_projectConfig.resolutionLabel())
        .arg(m_projectConfig.fps);
    if (!m_projectFilePath.isEmpty())
        title += " — " + QFileInfo(m_projectFilePath).fileName();
    setWindowTitle(title);
}

void MainWindow::applyProjectConfig(const ProjectConfig &config)
{
    m_projectConfig = config;
    m_player->setCanvasSize(config.width, config.height);
    updateTitle();
    statusBar()->showMessage(QString("Project: %1 — %2 %3fps")
        .arg(config.name).arg(config.resolutionLabel()).arg(config.fps));
}

void MainWindow::collectAudioState(ProjectData &data)
{
    if (auto *mixer = m_player ? m_player->audioMixer() : nullptr) {
        const int n = m_timeline ? m_timeline->audioTrackCount() : 0;
        data.trackEqStates.resize(n);
        for (int i = 0; i < n; ++i) {
            AudioEQConfig cfg = mixer->trackEqConfig(i);
            TrackEqState &s = data.trackEqStates[i];
            s.trackIdx = i;
            if (cfg.bands.size() > 0) {
                s.low = cfg.bands[0].gain;
                s.lowFreqHz = cfg.bands[0].frequency;
            }
            if (cfg.bands.size() > 1) {
                s.mid = cfg.bands[1].gain;
                s.midFreqHz = cfg.bands[1].frequency;
            }
            if (cfg.bands.size() > 2) {
                s.high = cfg.bands[2].gain;
                s.highFreqHz = cfg.bands[2].frequency;
            }
            s.qFactor = cfg.bands.isEmpty() ? 1.0 : cfg.bands[0].q;
            s.enabled = !cfg.isDefault();
        }
        const auto &cp = mixer->compressorParams();
        data.masterCompressor.thresholdDb = cp.thresholdDb;
        data.masterCompressor.ratio = cp.ratio;
        data.masterCompressor.attackMs = cp.attackMs;
        data.masterCompressor.releaseMs = cp.releaseMs;
        data.masterCompressor.makeupDb = cp.makeupDb;
        data.masterCompressor.enabled = cp.enabled;

        const auto &adParams = mixer->autoDuckParams();
        data.autoDuck.thresholdDb = adParams.thresholdDb;
        data.autoDuck.ratio = adParams.ratio;
        data.autoDuck.attackMs = adParams.attackMs;
        data.autoDuck.releaseMs = adParams.releaseMs;
    }
    data.audioMetersDockVisible = m_audioMetersDock ? m_audioMetersDock->isVisible() : true;
}

void MainWindow::applyAudioState(const ProjectData &data)
{
    if (auto *mixer = m_player ? m_player->audioMixer() : nullptr) {
        for (const auto &s : data.trackEqStates) {
            AudioEQConfig cfg;
            cfg.bands.resize(3);
            cfg.bands[0].frequency = s.lowFreqHz;
            cfg.bands[0].gain = s.low;
            cfg.bands[0].q = s.qFactor;
            cfg.bands[1].frequency = s.midFreqHz;
            cfg.bands[1].gain = s.mid;
            cfg.bands[1].q = s.qFactor;
            cfg.bands[2].frequency = s.highFreqHz;
            cfg.bands[2].gain = s.high;
            cfg.bands[2].q = s.qFactor;
            mixer->setTrackEqConfig(s.trackIdx, cfg);
            mixer->setTrackEqEnabled(s.trackIdx, s.enabled);
        }
        AudioMixer::CompressorParams cp;
        cp.thresholdDb = data.masterCompressor.thresholdDb;
        cp.ratio = data.masterCompressor.ratio;
        cp.attackMs = data.masterCompressor.attackMs;
        cp.releaseMs = data.masterCompressor.releaseMs;
        cp.makeupDb = data.masterCompressor.makeupDb;
        cp.enabled = data.masterCompressor.enabled;
        mixer->setCompressorParams(cp);

        AudioMixer::AutoDuckParams adParams;
        adParams.thresholdDb = data.autoDuck.thresholdDb;
        adParams.ratio = data.autoDuck.ratio;
        adParams.attackMs = data.autoDuck.attackMs;
        adParams.releaseMs = data.autoDuck.releaseMs;
        mixer->setAutoDuckParams(adParams);
    }
    if (m_audioMetersDock)
        m_audioMetersDock->setVisible(data.audioMetersDockVisible);
}

void MainWindow::newProject()
{
    ProjectSettingsDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        applyProjectConfig(dialog.config());
        hideWelcomeScreen();
        updateStatusInfo();
    }
}

void MainWindow::editProjectSettings()
{
    ProjectSettingsDialog dialog(this, &m_projectConfig);
    if (dialog.exec() == QDialog::Accepted) {
        applyProjectConfig(dialog.config());
        updateStatusInfo();
    }
}

void MainWindow::saveProject()
{
    if (m_projectFilePath.isEmpty()) {
        saveProjectAs();
        return;
    }

    ProjectData data;
    data.config = m_projectConfig;
    data.videoTracks = m_timeline->allVideoTracks();
    data.audioTracks = m_timeline->allAudioTracks();
    data.playheadPos = m_timeline->playheadPosition();
    data.markIn = m_timeline->markedIn();
    data.markOut = m_timeline->markedOut();
    data.zoomLevel = 10; // TODO: expose zoom level getter
    data.brushAnimations = m_brushAnimationEntries;

    collectAudioState(data);

    if (ProjectFile::save(m_projectFilePath, data)) {
        statusBar()->showMessage("Saved: " + m_projectFilePath);
        updateTitle();
    } else {
        QMessageBox::critical(this, "Save Failed", "Could not save project file.");
    }
}

void MainWindow::saveProjectAs()
{
    QString filePath = QFileDialog::getSaveFileName(this, "Save Project As",
        m_projectConfig.name + ".veditor", ProjectFile::fileFilter());
    if (filePath.isEmpty()) return;
    m_projectFilePath = filePath;
    saveProject();
}

void MainWindow::openProject()
{
    QString filePath = QFileDialog::getOpenFileName(this, "Open Project",
        QString(), ProjectFile::fileFilter());
    if (filePath.isEmpty()) return;

    ProjectData data;
    if (!ProjectFile::load(filePath, data)) {
        QMessageBox::critical(this, "Open Failed", "Could not load project file.");
        return;
    }

    m_projectFilePath = filePath;
    if (m_recentFilesManager)
        m_recentFilesManager->addFile(filePath);
    m_projectConfig = data.config;
    setBrushAnimationEntries(data.brushAnimations);
    if (m_player)
        m_player->setCanvasSize(data.config.width, data.config.height);
    if (m_timeline)
        m_timeline->restoreFromProject(data.videoTracks, data.audioTracks,
            data.playheadPos, data.markIn, data.markOut, data.zoomLevel);
    rebuildAudioMeters();
    applyAudioState(data);
    updateTitle();
    hideWelcomeScreen();
    updateStatusInfo();
    updateEditActions();
    syncBrushAnimationPreviewForClip(0, m_timeline ? m_timeline->selectedVideoClipIndex() : -1);
    statusBar()->showMessage("Opened: " + filePath);
}

void MainWindow::openFile()
{
    QString filter = "Video Files (*.mp4 *.mkv *.mov *.webm *.flv);;All Files (*)";
    QString filePath = QFileDialog::getOpenFileName(this, "Open Video", QString(), filter);
    if (!filePath.isEmpty())
        loadMediaFile(filePath, true, "Loaded");
}

void MainWindow::exportVideo()
{
    ExportDialog dialog(m_projectConfig, this);
    dialog.setSourceIsHdr(m_player && m_player->isHdrSource());
    if (dialog.exec() != QDialog::Accepted) return;

    ExportConfig exportCfg = dialog.config();
    const auto &clips = m_timeline->videoClips();

    if (clips.isEmpty()) {
        QMessageBox::warning(this, "Export", "No clips in timeline to export.");
        return;
    }

    auto *progress = new QProgressDialog("Exporting...", "Cancel", 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);

    connect(m_exporter, &Exporter::progressChanged, progress, &QProgressDialog::setValue);
    connect(m_exporter, &Exporter::exportFinished, this, [this, progress](bool success, const QString &msg) {
        progress->close();
        progress->deleteLater();
        if (success)
            statusBar()->showMessage(msg);
        else
            QMessageBox::critical(this, "Export Failed", msg);
    });
    connect(progress, &QProgressDialog::canceled, m_exporter, &Exporter::cancel);

    statusBar()->showMessage("Exporting: " + exportCfg.outputPath);
    m_exporter->startExport(exportCfg, clips);
}

void MainWindow::splitClip()
{
    m_timeline->splitAtPlayhead();
    statusBar()->showMessage("Split clip at playhead");
    updateEditActions();
}

void MainWindow::deleteClip()
{
    if (!m_timeline->hasSelection()) return;
    m_timeline->deleteSelectedClip();
    statusBar()->showMessage("Deleted clip");
    updateEditActions();
}

void MainWindow::rippleDelete()
{
    if (!m_timeline->hasSelection()) return;
    m_timeline->rippleDeleteSelectedClip();
    statusBar()->showMessage("Ripple deleted clip");
    updateEditActions();
}

void MainWindow::copyClip()
{
    m_timeline->copySelectedClip();
    statusBar()->showMessage("Copied clip to clipboard");
    updateEditActions();
}

void MainWindow::pasteClip()
{
    m_timeline->pasteClip();
    statusBar()->showMessage("Pasted clip");
    updateEditActions();
}

void MainWindow::copyEffects()
{
    if (!m_timeline || !m_timeline->hasSelection()) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }
    const int clipIdx = m_timeline->selectedVideoClipIndex();
    if (clipIdx < 0) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }
    const auto &clips = m_timeline->videoClips();
    const auto &clip = clips[clipIdx];

    effectctrl::ClipMotion motion;
    motion.scale = clip.videoScale;
    motion.dx = clip.videoDx;
    motion.dy = clip.videoDy;
    motion.rotationDeg = clip.rotation2DDegrees;
    motion.opacity = clip.opacity;

    effectctrl::EffectClipboard::instance().capture(clip.effects, motion);
    statusBar()->showMessage("Effects copied", 2000);
}

void MainWindow::pasteEffects()
{
    if (!m_timeline || !m_timeline->hasSelection()) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }
    auto &clipboard = effectctrl::EffectClipboard::instance();
    if (!clipboard.hasContent()) {
        statusBar()->showMessage("Clipboard is empty", 2000);
        return;
    }
    const int clipIdx = m_timeline->selectedVideoClipIndex();
    const int trackIdx = 0; // V1
    if (clipIdx < 0) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }

    m_timeline->setClipEffects(clipboard.effects());

    effectctrl::MotionState motion;
    motion.scale = clipboard.motion().scale;
    motion.dx = clipboard.motion().dx;
    motion.dy = clipboard.motion().dy;
    motion.rotation2DDeg = clipboard.motion().rotationDeg;
    motion.opacity = clipboard.motion().opacity;
    motion.is3DLayer = false;
    motion.posZ = 0.0;
    motion.rotX = 0.0;
    motion.rotY = 0.0;
    motion.rotZ = 0.0;
    m_timeline->setClipMotion(trackIdx, clipIdx, motion);

    statusBar()->showMessage("Effects pasted", 2000);
}

void MainWindow::pasteAttributes()
{
    if (!m_timeline || !m_timeline->hasSelection()) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }
    auto &clipboard = effectctrl::EffectClipboard::instance();
    if (!clipboard.hasContent()) {
        statusBar()->showMessage("Clipboard is empty", 2000);
        return;
    }
    const int clipIdx = m_timeline->selectedVideoClipIndex();
    const int trackIdx = 0;
    if (clipIdx < 0) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }
    const auto &clips = m_timeline->videoClips();
    auto targetClip = clips[clipIdx];

    PasteAttributesDialog dialog(clipboard.effects(), clipboard.motion(), this);
    if (dialog.exec() != QDialog::Accepted) return;

    auto sel = dialog.selection();

    if (sel.pastePosition || sel.pasteScale || sel.pasteRotation || sel.pasteOpacity) {
        effectctrl::MotionState motion;
        const auto &cbMotion = clipboard.motion();
        const auto &srcClip = targetClip;

        motion.scale = sel.pasteScale ? cbMotion.scale : srcClip.videoScale;
        motion.dx = sel.pastePosition ? cbMotion.dx : srcClip.videoDx;
        motion.dy = sel.pastePosition ? cbMotion.dy : srcClip.videoDy;
        motion.rotation2DDeg = sel.pasteRotation ? cbMotion.rotationDeg : srcClip.rotation2DDegrees;
        motion.opacity = sel.pasteOpacity ? cbMotion.opacity : srcClip.opacity;
        motion.is3DLayer = srcClip.is3DLayer;
        motion.posZ = srcClip.layer3D.positionZ;
        motion.rotX = srcClip.layer3D.rotationX;
        motion.rotY = srcClip.layer3D.rotationY;
        motion.rotZ = srcClip.layer3D.rotationZ;

        m_timeline->setClipMotion(trackIdx, clipIdx, motion);
    }

    if (!sel.effectIndices.isEmpty()) {
        auto targetEffects = targetClip.effects;
        const auto &cbEffects = clipboard.effects();

        targetEffects.clear();
        for (int idx : sel.effectIndices) {
            if (idx >= 0 && idx < cbEffects.size())
                targetEffects.append(cbEffects[idx]);
        }

        m_timeline->setClipEffects(targetEffects);
    }

    statusBar()->showMessage("Attributes past", 2000);
}

void MainWindow::undoAction()
{
    m_timeline->undo();
    statusBar()->showMessage("Undo");
    updateEditActions();
}

void MainWindow::redoAction()
{
    m_timeline->redo();
    statusBar()->showMessage("Redo");
    updateEditActions();
}

void MainWindow::toggleSnap()
{
    bool snap = !m_timeline->snapEnabled();
    m_timeline->setSnapEnabled(snap);
    m_snapAction->setChecked(snap);
    statusBar()->showMessage(snap ? "Snap enabled" : "Snap disabled");
}

void MainWindow::zoomIn()
{
    m_timeline->zoomIn();
    statusBar()->showMessage(QString("Zoom: %1 px/s").arg(m_timeline->videoClips().isEmpty() ? 15 : 15));
}

void MainWindow::zoomOut()
{
    m_timeline->zoomOut();
    statusBar()->showMessage("Zoom out");
}

void MainWindow::addVideoTrack()
{
    m_timeline->addVideoTrack();
    statusBar()->showMessage(QString("Added video track V%1").arg(m_timeline->videoTrackCount()));
}

void MainWindow::addAudioTrack()
{
    m_timeline->addAudioTrack();
    rebuildAudioMeters();
    statusBar()->showMessage(QString("Added audio track A%1").arg(m_timeline->audioTrackCount()));
}

void MainWindow::markIn()
{
    m_timeline->markIn();
    statusBar()->showMessage(QString("Mark In: %1s").arg(m_timeline->markedIn(), 0, 'f', 1));
}

void MainWindow::markOut()
{
    m_timeline->markOut();
    statusBar()->showMessage(QString("Mark Out: %1s").arg(m_timeline->markedOut(), 0, 'f', 1));
}

void MainWindow::setClipSpeed()
{
    if (!m_timeline->hasSelection()) return;
    bool ok;
    double speed = QInputDialog::getDouble(this, "Set Clip Speed",
        "Speed (0.25x - 4.0x):", 1.0, 0.25, 4.0, 2, &ok);
    if (ok) {
        m_timeline->setClipSpeed(speed);
        statusBar()->showMessage(QString("Clip speed: %1x").arg(speed));
    }
}

void MainWindow::setClipVolume()
{
    if (!m_timeline->hasSelection()) return;
    bool ok;
    double vol = QInputDialog::getDouble(this, "Set Clip Volume",
        "Volume (0% = mute, 100% = normal, 200% = boost):", 100.0, 0.0, 200.0, 0, &ok);
    if (ok) {
        m_timeline->setClipVolume(vol / 100.0);
        statusBar()->showMessage(QString("Volume: %1%").arg(static_cast<int>(vol)));
    }
}

void MainWindow::addBgm()
{
    QString filter = "Audio Files (*.mp3 *.wav *.aac *.ogg *.flac *.m4a);;All Files (*)";
    QString filePath = QFileDialog::getOpenFileName(this, "Add BGM / Audio", QString(), filter);
    if (!filePath.isEmpty()) {
        // Ensure we have a second audio track for BGM
        if (m_timeline->audioTrackCount() < 2)
            m_timeline->addAudioTrack();
        m_timeline->addAudioFile(filePath);
        statusBar()->showMessage("Added BGM: " + filePath);
    }
}

void MainWindow::toggleMute()
{
    m_timeline->toggleMuteTrack(0);
    statusBar()->showMessage(QString("A1: %1").arg(m_timeline->audioTrackCount() > 0 ? "Toggled mute" : "No track"));
}

void MainWindow::toggleSolo()
{
    m_timeline->toggleSoloTrack(0);
    statusBar()->showMessage("A1: Toggled solo");
}

void MainWindow::setupToolPropertyPanel()
{
    m_toolPropertyStack = new QStackedWidget(this);
    m_toolPropertyStack->setMinimumWidth(260);
    m_toolPropertyStack->setMaximumWidth(360);

    // Page 0: empty placeholder shown when no tool is active.
    auto *emptyPage = new QWidget(m_toolPropertyStack);
    auto *emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->addStretch();
    auto *emptyLabel = new QLabel("ツール未選択", emptyPage);
    emptyLabel->setAlignment(Qt::AlignCenter);
    emptyLabel->setStyleSheet("color: #888;");
    emptyLayout->addWidget(emptyLabel);
    emptyLayout->addStretch();
    m_toolPropertyStack->addWidget(emptyPage);

    // Page 1: text tool properties — text / font size / color / 適用.
    auto *textPage = new QWidget(m_toolPropertyStack);
    auto *textLayout = new QVBoxLayout(textPage);
    textLayout->setContentsMargins(12, 12, 12, 12);
    auto *titleLabel = new QLabel("テキストツール", textPage);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 1);
    titleLabel->setFont(titleFont);
    textLayout->addWidget(titleLabel);
    textLayout->addSpacing(8);

    auto *form = new QFormLayout();
    m_textToolLineEdit = new QLineEdit(textPage);
    m_textToolLineEdit->setPlaceholderText("テキストを入力...");
    form->addRow("テキスト", m_textToolLineEdit);

    m_textToolSizeSpin = new QSpinBox(textPage);
    m_textToolSizeSpin->setRange(6, 256);
    m_textToolSizeSpin->setValue(32);
    m_textToolSizeSpin->setSuffix(" pt");
    connect(m_textToolSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { pushTextToolStyleToPreview(); });
    form->addRow("サイズ", m_textToolSizeSpin);

    m_textToolColor = Qt::white;
    m_textToolColorButton = new QPushButton(textPage);
    m_textToolColorButton->setText("色を選択...");
    m_textToolColorButton->setStyleSheet("background-color: white; color: black;");
    connect(m_textToolColorButton, &QPushButton::clicked, this, [this]() {
        QColor picked = QColorDialog::getColor(m_textToolColor, this, "テキスト色");
        if (picked.isValid()) {
            m_textToolColor = picked;
            m_textToolColorButton->setStyleSheet(
                QString("background-color: %1; color: %2;")
                    .arg(picked.name())
                    .arg(picked.lightness() > 128 ? "black" : "white"));
            pushTextToolStyleToPreview();
        }
    });
    form->addRow("色", m_textToolColorButton);

    m_textToolStartSpin = new QDoubleSpinBox(textPage);
    m_textToolStartSpin->setRange(0.0, 36000.0);
    m_textToolStartSpin->setDecimals(2);
    m_textToolStartSpin->setSingleStep(0.5);
    m_textToolStartSpin->setSuffix(" s");
    m_textToolStartSpin->setValue(0.0);
    form->addRow("開始時間", m_textToolStartSpin);

    // 表示時間 = duration (not absolute end time). applyTextToolOverlay
    // computes overlay.endTime = startTime + duration so the downstream
    // renderer still gets an absolute end time in EnhancedTextOverlay.
    m_textToolEndSpin = new QDoubleSpinBox(textPage);
    m_textToolEndSpin->setRange(0.1, 36000.0);
    m_textToolEndSpin->setDecimals(2);
    m_textToolEndSpin->setSingleStep(0.5);
    m_textToolEndSpin->setSuffix(" s");
    m_textToolEndSpin->setValue(5.0);
    form->addRow("表示時間", m_textToolEndSpin);

    // Gradient fill controls (read by applyTextToolOverlay on 適用).
    m_textToolGradientCheck = new QCheckBox("グラデーション", textPage);
    form->addRow("", m_textToolGradientCheck);
    m_textToolGradientStart = Qt::white;
    m_textToolGradientEnd   = QColor(255, 200, 0);
    auto styleColorBtn = [](QPushButton *b, const QColor &c) {
        b->setStyleSheet(QString("background-color: %1; color: %2;")
                             .arg(c.name())
                             .arg(c.lightness() > 128 ? "black" : "white"));
    };
    m_textToolGradientStartBtn = new QPushButton("開始色", textPage);
    styleColorBtn(m_textToolGradientStartBtn, m_textToolGradientStart);
    connect(m_textToolGradientStartBtn, &QPushButton::clicked, this, [this, styleColorBtn]() {
        QColor picked = QColorDialog::getColor(m_textToolGradientStart, this, "グラデーション開始色");
        if (picked.isValid()) {
            m_textToolGradientStart = picked;
            styleColorBtn(m_textToolGradientStartBtn, picked);
        }
    });
    form->addRow("開始色", m_textToolGradientStartBtn);
    m_textToolGradientEndBtn = new QPushButton("終了色", textPage);
    styleColorBtn(m_textToolGradientEndBtn, m_textToolGradientEnd);
    connect(m_textToolGradientEndBtn, &QPushButton::clicked, this, [this, styleColorBtn]() {
        QColor picked = QColorDialog::getColor(m_textToolGradientEnd, this, "グラデーション終了色");
        if (picked.isValid()) {
            m_textToolGradientEnd = picked;
            styleColorBtn(m_textToolGradientEndBtn, picked);
        }
    });
    form->addRow("終了色", m_textToolGradientEndBtn);
    m_textToolGradientAngleSpin = new QDoubleSpinBox(textPage);
    m_textToolGradientAngleSpin->setRange(0.0, 360.0);
    m_textToolGradientAngleSpin->setDecimals(0);
    m_textToolGradientAngleSpin->setSingleStep(15.0);
    m_textToolGradientAngleSpin->setSuffix(" °");
    m_textToolGradientAngleSpin->setValue(90.0);
    form->addRow("角度", m_textToolGradientAngleSpin);

    // Adobe-style fine controls: type (Linear/Radial), midpoint, reverse.
    m_textToolGradientTypeCombo = new QComboBox(textPage);
    m_textToolGradientTypeCombo->addItem("線形", 0);
    m_textToolGradientTypeCombo->addItem("放射状", 1);
    form->addRow("種類", m_textToolGradientTypeCombo);

    m_textToolGradientMidSpin = new QDoubleSpinBox(textPage);
    m_textToolGradientMidSpin->setRange(1.0, 99.0);
    m_textToolGradientMidSpin->setDecimals(0);
    m_textToolGradientMidSpin->setSingleStep(5.0);
    m_textToolGradientMidSpin->setSuffix(" %");
    m_textToolGradientMidSpin->setValue(50.0);
    form->addRow("中点", m_textToolGradientMidSpin);

    m_textToolGradientReverseCheck = new QCheckBox("反転", textPage);
    form->addRow("", m_textToolGradientReverseCheck);

    // Illustrator-style multi-stop editor: horizontal gradient bar with
    // draggable markers. Click empty area to add, right-click to delete.
    m_textToolStopBar = new GradientStopBar(textPage);
    form->addRow("ストップ", m_textToolStopBar);

    // Per-stop property controls (active stop is set by GradientStopBar::stopSelected).
    m_textToolStopColorBtn = new QPushButton("色を選択…", textPage);
    styleColorBtn(m_textToolStopColorBtn, Qt::white);
    connect(m_textToolStopColorBtn, &QPushButton::clicked, this, [this, styleColorBtn]() {
        if (!m_textToolStopBar) return;
        const int idx = m_textToolStopBar->selectedIndex();
        if (idx < 0 || idx >= m_textToolStopBar->stops().size()) return;
        QColor picked = QColorDialog::getColor(m_textToolStopBar->stops()[idx].color,
                                               this, "ストップ色");
        if (!picked.isValid()) return;
        GradientStop s = m_textToolStopBar->stops()[idx];
        s.color = picked;
        m_textToolStopBar->updateStop(idx, s);
        styleColorBtn(m_textToolStopColorBtn, picked);
        pushTextToolStyleToPreview();
    });
    form->addRow("ストップ色", m_textToolStopColorBtn);

    m_textToolStopOpacitySpin = new QDoubleSpinBox(textPage);
    m_textToolStopOpacitySpin->setRange(0.0, 100.0);
    m_textToolStopOpacitySpin->setDecimals(0);
    m_textToolStopOpacitySpin->setSingleStep(5.0);
    m_textToolStopOpacitySpin->setSuffix(" %");
    m_textToolStopOpacitySpin->setValue(100.0);
    connect(m_textToolStopOpacitySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!m_textToolStopBar) return;
                const int idx = m_textToolStopBar->selectedIndex();
                if (idx < 0 || idx >= m_textToolStopBar->stops().size()) return;
                GradientStop s = m_textToolStopBar->stops()[idx];
                s.opacity = qBound(0.0, v / 100.0, 1.0);
                m_textToolStopBar->updateStop(idx, s);
                pushTextToolStyleToPreview();
            });
    form->addRow("ストップ不透明度", m_textToolStopOpacitySpin);

    m_textToolStopPosSpin = new QDoubleSpinBox(textPage);
    m_textToolStopPosSpin->setRange(0.0, 100.0);
    m_textToolStopPosSpin->setDecimals(0);
    m_textToolStopPosSpin->setSingleStep(1.0);
    m_textToolStopPosSpin->setSuffix(" %");
    m_textToolStopPosSpin->setValue(0.0);
    connect(m_textToolStopPosSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!m_textToolStopBar) return;
                const int idx = m_textToolStopBar->selectedIndex();
                if (idx < 0 || idx >= m_textToolStopBar->stops().size()) return;
                GradientStop s = m_textToolStopBar->stops()[idx];
                s.position = qBound(0.0, v / 100.0, 1.0);
                m_textToolStopBar->updateStop(idx, s);
                pushTextToolStyleToPreview();
            });
    form->addRow("ストップ位置", m_textToolStopPosSpin);

    // Sync per-stop controls when a stop is selected on the bar.
    connect(m_textToolStopBar, &GradientStopBar::stopSelected, this, [this, styleColorBtn](int idx) {
        if (!m_textToolStopBar || idx < 0 || idx >= m_textToolStopBar->stops().size()) return;
        const GradientStop &s = m_textToolStopBar->stops()[idx];
        if (m_textToolStopColorBtn) styleColorBtn(m_textToolStopColorBtn, s.color);
        if (m_textToolStopOpacitySpin) {
            QSignalBlocker b(m_textToolStopOpacitySpin);
            m_textToolStopOpacitySpin->setValue(s.opacity * 100.0);
        }
        if (m_textToolStopPosSpin) {
            QSignalBlocker b(m_textToolStopPosSpin);
            m_textToolStopPosSpin->setValue(s.position * 100.0);
        }
    });
    connect(m_textToolStopBar, &GradientStopBar::stopsChanged, this, [this]() {
        pushTextToolStyleToPreview();
    });

    // Gradient preset save / load buttons — JSON files under AppData/gradients.
    m_textToolGradientPresetSaveBtn = new QPushButton("保存", textPage);
    m_textToolGradientPresetLoadBtn = new QPushButton("呼び出し", textPage);
    connect(m_textToolGradientPresetSaveBtn, &QPushButton::clicked, this, [this]() {
        if (!m_textToolStopBar) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "プリセット保存",
                                                   "名前:", QLineEdit::Normal, "preset", &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/gradients";
        QDir().mkpath(dir);
        QJsonArray stopsArr;
        for (const auto &s : m_textToolStopBar->stops()) {
            QJsonObject so;
            so["position"] = s.position;
            so["color"] = s.color.name(QColor::HexArgb);
            so["opacity"] = s.opacity;
            stopsArr.append(so);
        }
        QJsonObject root;
        root["name"] = name.trimmed();
        root["type"] = m_textToolGradientTypeCombo ? m_textToolGradientTypeCombo->currentData().toInt() : 0;
        root["angle"] = m_textToolGradientAngleSpin ? m_textToolGradientAngleSpin->value() : 90.0;
        root["reverse"] = m_textToolGradientReverseCheck && m_textToolGradientReverseCheck->isChecked();
        root["stops"] = stopsArr;
        const QString path = dir + "/" + name.trimmed() + ".json";
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(root).toJson());
            statusBar()->showMessage(QString("プリセット保存: %1").arg(path));
        } else {
            statusBar()->showMessage("プリセット保存失敗");
        }
    });
    connect(m_textToolGradientPresetLoadBtn, &QPushButton::clicked, this, [this]() {
        if (!m_textToolStopBar) return;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/gradients";
        QDir().mkpath(dir);
        const QString path = QFileDialog::getOpenFileName(this, "プリセット呼び出し", dir, "JSON (*.json)");
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return;
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        if (m_textToolGradientTypeCombo) {
            const int t = root["type"].toInt(0);
            const int idx = m_textToolGradientTypeCombo->findData(t);
            if (idx >= 0) m_textToolGradientTypeCombo->setCurrentIndex(idx);
        }
        if (m_textToolGradientAngleSpin)
            m_textToolGradientAngleSpin->setValue(root["angle"].toDouble(90.0));
        if (m_textToolGradientReverseCheck)
            m_textToolGradientReverseCheck->setChecked(root["reverse"].toBool(false));
        QVector<GradientStop> stops;
        for (const auto &v : root["stops"].toArray()) {
            const QJsonObject so = v.toObject();
            GradientStop s;
            s.position = so["position"].toDouble(0.0);
            s.color = QColor(so["color"].toString("#ffffffff"));
            s.opacity = so["opacity"].toDouble(1.0);
            stops.append(s);
        }
        if (!stops.isEmpty())
            m_textToolStopBar->setStops(stops);
        pushTextToolStyleToPreview();
        statusBar()->showMessage(QString("プリセット呼び出し: %1").arg(path));
    });
    auto *presetRow = new QHBoxLayout();
    presetRow->addWidget(m_textToolGradientPresetSaveBtn);
    presetRow->addWidget(m_textToolGradientPresetLoadBtn);
    form->addRow("プリセット", presetRow);

    // Outline stroke controls.
    m_textToolOutlineCheck = new QCheckBox("枠線", textPage);
    form->addRow("", m_textToolOutlineCheck);
    m_textToolOutlineColor = Qt::black;
    m_textToolOutlineColorBtn = new QPushButton("枠線色", textPage);
    styleColorBtn(m_textToolOutlineColorBtn, m_textToolOutlineColor);
    connect(m_textToolOutlineColorBtn, &QPushButton::clicked, this, [this, styleColorBtn]() {
        QColor picked = QColorDialog::getColor(m_textToolOutlineColor, this, "枠線色");
        if (picked.isValid()) {
            m_textToolOutlineColor = picked;
            styleColorBtn(m_textToolOutlineColorBtn, picked);
        }
    });
    form->addRow("枠線色", m_textToolOutlineColorBtn);
    m_textToolOutlineWidthSpin = new QSpinBox(textPage);
    m_textToolOutlineWidthSpin->setRange(0, 20);
    m_textToolOutlineWidthSpin->setValue(2);
    m_textToolOutlineWidthSpin->setSuffix(" px");
    form->addRow("枠線幅", m_textToolOutlineWidthSpin);

    textLayout->addLayout(form);
    textLayout->addSpacing(12);

    auto *applyButton = new QPushButton("適用", textPage);
    applyButton->setMinimumHeight(32);
    connect(applyButton, &QPushButton::clicked, this, &MainWindow::applyTextToolOverlay);
    textLayout->addWidget(applyButton);

    auto *hint = new QLabel(
        "プレビュー上でドラッグしてテキスト枠を指定してください。\n"
        "ドラッグしない場合は中央に配置されます。", textPage);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #888; font-size: 11px;");
    textLayout->addWidget(hint);
    textLayout->addStretch();

    // The text panel grew tall with gradient/stop controls — wrap it in a
    // scroll area so it doesn't force the main window to an abnormal height
    // on smaller screens. The scroll area takes the panel slot in the stack.
    auto *textScroll = new QScrollArea(m_toolPropertyStack);
    textScroll->setWidget(textPage);
    textScroll->setWidgetResizable(true);
    textScroll->setFrameShape(QFrame::NoFrame);
    textScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_toolPropertyStack->addWidget(textScroll);
    m_toolPropertyStack->setCurrentIndex(0);
}

void MainWindow::onTextToolToggled(bool checked)
{
    m_textToolActive = checked;
    if (m_player)
        m_player->setTextToolActive(checked);
    if (!m_toolPropertyStack)
        return;
    if (checked) {
        m_toolPropertyStack->setCurrentIndex(1);
        m_toolPropertyStack->show();
        pushTextToolStyleToPreview();
        // Pre-fill time defaults: 開始時間 = current playhead, 表示時間 = 5 s.
        // Both remain adjustable via the spinboxes before 適用.
        if (m_timeline && m_textToolStartSpin && m_textToolEndSpin) {
            m_textToolStartSpin->setValue(m_timeline->playheadPosition());
            m_textToolEndSpin->setValue(5.0);
        }
        statusBar()->showMessage("テキストツール ON — プレビュー上でドラッグして枠を指定、その場で直接入力");
    } else {
        m_toolPropertyStack->setCurrentIndex(0);
        m_toolPropertyStack->hide();
        m_textToolHasPendingRect = false;
        statusBar()->showMessage("テキストツール OFF");
    }
}

void MainWindow::onTextRectRequested(const QRectF &normalizedRect)
{
    m_textToolPendingRect = normalizedRect;
    m_textToolHasPendingRect = true;
    pushTextToolStyleToPreview();
    // Refresh 開始時間 against the current playhead each time a fresh rect
    // is drawn — makes the text default to "appears at the current playhead
    // for 5 s". The duration spin is left alone so a user-specified value
    // carries forward across multiple rect draws.
    if (m_timeline && m_textToolStartSpin)
        m_textToolStartSpin->setValue(m_timeline->playheadPosition());
    statusBar()->showMessage(QString("テキスト枠指定: %1,%2 %3x%4 — プレビュー上で直接入力するか、右パネルで『適用』")
        .arg(normalizedRect.x(), 0, 'f', 2)
        .arg(normalizedRect.y(), 0, 'f', 2)
        .arg(normalizedRect.width(), 0, 'f', 2)
        .arg(normalizedRect.height(), 0, 'f', 2));
}

void MainWindow::onTextInlineCommitted(const QString &text, const QRectF &normalizedRect)
{
    // Populate the pending state from the inline commit and run the normal
    // apply path so the overlay inherits the right panel's size/color/time
    // values. The QLineEdit is filled only so applyTextToolOverlay's empty
    // guard passes — it's cleared again at the end of the apply.
    m_textToolPendingRect = normalizedRect;
    m_textToolHasPendingRect = true;
    if (m_textToolLineEdit)
        m_textToolLineEdit->setText(text);
    applyTextToolOverlay();
}

void MainWindow::onTextOverlayEditCommitted(int overlayIndex, const QString &newText)
{
    // Click-to-edit on an existing overlay — only the text string changes,
    // the rect / style / time range stay put. Push the updated overlay list
    // back to the player so the preview re-renders with the new text.
    if (!m_timeline->updateTextOverlayText(overlayIndex, newText)) {
        statusBar()->showMessage("テキスト更新失敗");
        return;
    }
    const auto &clips = m_timeline->videoClips();
    if (!clips.isEmpty() && m_player) {
        QVector<EnhancedTextOverlay> overlays;
        const auto &mgr = clips[0].textManager;
        for (int i = 0; i < mgr.count(); ++i)
            overlays.append(mgr.overlay(i));
        m_player->setTextOverlays(overlays);
    }
    if (m_player)
        m_player->clearTextToolRect();
    statusBar()->showMessage(QString("テキスト更新: 「%1」").arg(newText));
}

void MainWindow::pushTextToolStyleToPreview()
{
    if (!m_player)
        return;
    // Use Arial + bold to match EnhancedTextOverlay's default font family;
    // inheriting the system default here was causing WYSIWYG metric drift
    // between the inline edit layer and composeFrameWithOverlays.
    QFont f("Arial");
    f.setPointSize(m_textToolSizeSpin ? m_textToolSizeSpin->value() : 32);
    f.setBold(true);
    m_player->setTextToolStyle(f, m_textToolColor);
}

void MainWindow::applyTextToolOverlay()
{
    // 適用 primarily commits whatever the user has been typing directly on
    // the preview (Adobe-style in-place edit). If the GLPreview is in edit
    // mode with non-empty text, just delegate: its commitCurrentTextToolEdit
    // fires the textInlineCommitted / textOverlayEditCommitted signal which
    // lands on the normal slot and runs the right-panel apply from there.
    if (m_player && m_player->isTextToolEditing()
        && !m_player->currentTextToolInputText().isEmpty()) {
        m_player->commitCurrentTextToolEdit();
        return;
    }
    if (!m_textToolLineEdit || m_textToolLineEdit->text().isEmpty()) {
        statusBar()->showMessage("テキストが空です");
        return;
    }
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        statusBar()->showMessage("先にクリップを選択してください");
        return;
    }

    EnhancedTextOverlay overlay;
    overlay.text = m_textToolLineEdit->text();
    QFont font = overlay.font;
    font.setPointSize(m_textToolSizeSpin ? m_textToolSizeSpin->value() : 32);
    overlay.font = font;
    overlay.color = m_textToolColor;
    // Transparent background by default — the user explicitly asked for it
    // so the text sits directly on the video instead of on a translucent box.
    overlay.backgroundColor = QColor(0, 0, 0, 0);
    overlay.gradientEnabled = m_textToolGradientCheck && m_textToolGradientCheck->isChecked();
    overlay.gradientStart = m_textToolGradientStart;
    overlay.gradientEnd = m_textToolGradientEnd;
    overlay.gradientAngle = m_textToolGradientAngleSpin ? m_textToolGradientAngleSpin->value() : 90.0;
    overlay.gradientType = m_textToolGradientTypeCombo ? m_textToolGradientTypeCombo->currentData().toInt() : 0;
    overlay.gradientMidpoint = m_textToolGradientMidSpin ? m_textToolGradientMidSpin->value() : 50.0;
    overlay.gradientReverse = m_textToolGradientReverseCheck && m_textToolGradientReverseCheck->isChecked();
    if (m_textToolStopBar)
        overlay.gradientStops = m_textToolStopBar->stops();
    if (m_textToolOutlineCheck && m_textToolOutlineCheck->isChecked()) {
        overlay.outlineColor = m_textToolOutlineColor;
        overlay.outlineWidth = m_textToolOutlineWidthSpin ? m_textToolOutlineWidthSpin->value() : 2;
    } else {
        overlay.outlineWidth = 0;
    }
    overlay.startTime = m_textToolStartSpin ? m_textToolStartSpin->value() : 0.0;
    // The second spin is 表示時間 (duration), not absolute end time — the
    // user asked for duration semantics so the text defaults to "now + 5 s".
    const double duration = m_textToolEndSpin ? m_textToolEndSpin->value() : 5.0;
    overlay.endTime = overlay.startTime + qMax(0.1, duration);
    if (m_textToolHasPendingRect) {
        overlay.x = m_textToolPendingRect.x() + m_textToolPendingRect.width() / 2.0;
        overlay.y = m_textToolPendingRect.y() + m_textToolPendingRect.height() / 2.0;
        overlay.width = m_textToolPendingRect.width();
        overlay.height = m_textToolPendingRect.height();
    }

    if (!m_timeline->addTextOverlayToFirstVideoClip(overlay)) {
        statusBar()->showMessage("テキスト追加失敗 — クリップが見つかりません");
        return;
    }

    // Push the updated overlay list to the preview so the new text is
    // visible immediately. MainWindow is the single source of truth that
    // owns the timeline → player forwarding.
    const auto &clips = m_timeline->videoClips();
    if (!clips.isEmpty()) {
        QVector<EnhancedTextOverlay> overlays;
        const auto &mgr = clips[0].textManager;
        for (int i = 0; i < mgr.count(); ++i)
            overlays.append(mgr.overlay(i));
        if (m_player)
            m_player->setTextOverlays(overlays);
    }
    statusBar()->showMessage(QString("テキストを追加しました: 「%1」").arg(overlay.text));

    m_textToolLineEdit->clear();
    m_textToolHasPendingRect = false;
    if (m_player)
        m_player->clearTextToolRect();
}

void MainWindow::addTextOverlay()
{
    TextOverlayDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        auto overlay = dialog.result();
        // TODO: Store overlay in project and render on preview
        statusBar()->showMessage(QString("Added text: \"%1\"").arg(overlay.text));
    }
}

void MainWindow::exportTextOverlays()
{
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "テキスト書き出し",
                                 "クリップにテキストオーバーレイがありません。");
        return;
    }
    const auto &clip = m_timeline->videoClips().first();
    if (clip.textManager.count() == 0) {
        QMessageBox::information(this, "テキスト書き出し",
                                 "V1 の先頭クリップにテキストがありません。");
        return;
    }
    QString selectedFilter;
    const QString path = QFileDialog::getSaveFileName(
        this, "テキストを書き出し", QString(),
        "SubRip (*.srt);;CSV (*.csv);;All Files (*)", &selectedFilter);
    if (path.isEmpty())
        return;

    QVector<EnhancedTextOverlay> overlays;
    for (int i = 0; i < clip.textManager.count(); ++i)
        overlays.append(clip.textManager.overlay(i));

    const bool wantCsv = selectedFilter.contains("CSV")
                      || path.endsWith(".csv", Qt::CaseInsensitive);
    const bool ok = wantCsv
        ? TextManager::exportCSV(overlays, path)
        : TextManager::exportSRT(overlays, path);
    if (ok)
        statusBar()->showMessage(QString("%1 にテキストを書き出しました").arg(path));
    else
        QMessageBox::warning(this, "テキスト書き出し",
                             QString("書き出しに失敗しました: %1").arg(path));
}

void MainWindow::manageTextOverlays()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Text Overlays", "Select a clip first.");
        return;
    }

    // Get current clip's text overlays
    int sel = m_timeline->videoClips().size() > 0 ? 0 : -1;
    if (sel < 0) return;

    auto clips = m_timeline->videoClips();
    auto &textMgr = clips[sel].textManager;

    QString info = QString("Current clip has %1 text overlay(s).\n\n").arg(textMgr.count());
    for (int i = 0; i < textMgr.count(); ++i) {
        const auto &o = textMgr.overlay(i);
        info += QString("%1. \"%2\" (%3s - %4s)\n")
            .arg(i + 1)
            .arg(o.text.left(30))
            .arg(o.startTime, 0, 'f', 1)
            .arg(o.endTime > 0 ? QString::number(o.endTime, 'f', 1) : "end");
    }

    QMessageBox::information(this, "Text Overlays", info);
}

void MainWindow::importSubtitles()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Import Subtitles", "Select a clip first.");
        return;
    }

    QString filter = "Subtitle Files (*.srt *.vtt);;SRT (*.srt);;WebVTT (*.vtt);;All Files (*)";
    QString filePath = QFileDialog::getOpenFileName(this, "Import Subtitles", QString(), filter);
    if (filePath.isEmpty()) return;

    QVector<EnhancedTextOverlay> overlays;
    if (filePath.endsWith(".srt", Qt::CaseInsensitive))
        overlays = TextManager::importSRT(filePath);
    else if (filePath.endsWith(".vtt", Qt::CaseInsensitive))
        overlays = TextManager::importVTT(filePath);

    if (overlays.isEmpty()) {
        QMessageBox::warning(this, "Import", "No subtitles found in file.");
        return;
    }

    // Add to current clip
    auto clips = m_timeline->videoClips();
    if (!clips.isEmpty()) {
        for (const auto &o : overlays)
            clips[0].textManager.addOverlay(o);
        // Update via timeline
    }

    statusBar()->showMessage(QString("Imported %1 subtitle(s) from %2")
        .arg(overlays.size()).arg(QFileInfo(filePath).fileName()));
}

void MainWindow::saveTextTemplate()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Save Template", "Select a clip with text overlays first.");
        return;
    }

    bool ok;
    QString name = QInputDialog::getText(this, "Save Text Template",
        "Template name:", QLineEdit::Normal, "My Template", &ok);
    if (!ok || name.isEmpty()) return;

    // Create template from default style
    EnhancedTextOverlay sample;
    TextTemplate tmpl = TextTemplate::fromOverlay(sample, name);
    TextManager::saveTemplate(tmpl);
    statusBar()->showMessage(QString("Template saved: %1").arg(name));
}

void MainWindow::addTransition()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Transition", "Select a clip first to add a transition.");
        return;
    }
    TransitionDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        auto transition = dialog.result();
        m_timeline->applyTransitionToSelected(transition);
        statusBar()->showMessage(QString("Added transition: %1 (%2s)")
            .arg(Transition::typeName(transition.type)).arg(transition.duration));
    }
}

void MainWindow::applyDefaultTransition()
{
    if (!m_timeline->hasSelection()) {
        statusBar()->showMessage("Select a clip first", 2000);
        return;
    }
    QSettings prefs("VSimpleEditor", "Preferences");
    const int rawType = prefs.value("transition/defaultType",
        static_cast<int>(TransitionType::CrossDissolve)).toInt();
    const double duration = qBound(0.1,
        prefs.value("transition/defaultDuration", 1.0).toDouble(), 5.0);
    const int rawEasing = prefs.value("transition/defaultEasing",
        static_cast<int>(TransitionEasing::Linear)).toInt();
    Transition t;
    t.type = static_cast<TransitionType>(rawType);
    t.duration = duration;
    t.easing = static_cast<TransitionEasing>(rawEasing);
    m_timeline->applyTransitionToSelected(t);
    statusBar()->showMessage(QString("Applied default transition: %1 (%2s, %3)")
        .arg(Transition::typeName(t.type))
        .arg(t.duration, 0, 'f', 1)
        .arg(Transition::easingName(t.easing)), 2000);
}

void MainWindow::editDefaultTransition()
{
    QSettings prefs("VSimpleEditor", "Preferences");
    const int curType = prefs.value("transition/defaultType",
        static_cast<int>(TransitionType::CrossDissolve)).toInt();
    const double curDuration = prefs.value("transition/defaultDuration", 1.0).toDouble();
    const int curEasing = prefs.value("transition/defaultEasing",
        static_cast<int>(TransitionEasing::Linear)).toInt();

    QDialog dialog(this);
    dialog.setWindowTitle("規定トランジション設定");
    auto *form = new QFormLayout(&dialog);

    auto *typeCombo = new QComboBox(&dialog);
    const TransitionType options[] = {
        TransitionType::CrossDissolve,
        TransitionType::FadeIn,
        TransitionType::FadeOut,
        TransitionType::DipToBlack,
        TransitionType::DipToWhite,
        TransitionType::WipeLeft,
        TransitionType::WipeRight,
        TransitionType::WipeUp,
        TransitionType::WipeDown,
        TransitionType::ClockWipe,
        TransitionType::BarnDoorHorizontal,
        TransitionType::BarnDoorVertical,
        TransitionType::SlideLeft,
        TransitionType::SlideRight,
        TransitionType::SlideUp,
        TransitionType::SlideDown,
        TransitionType::PushLeft,
        TransitionType::PushRight,
        TransitionType::PushUp,
        TransitionType::PushDown,
        TransitionType::CrossZoom,
        TransitionType::FilmDissolve,
        TransitionType::SpinCW,
        TransitionType::SpinCCW,
        TransitionType::DitherDissolve,
        TransitionType::BlurDissolve,
        TransitionType::Pixelate,
        TransitionType::WhipPanLeft,
        TransitionType::WhipPanRight,
        TransitionType::Glitch,
        TransitionType::LightLeak,
        TransitionType::LensFlare,
        TransitionType::FilmBurn,
        TransitionType::CameraShake,
        TransitionType::ColorChannelShift,
        TransitionType::FlipHorizontal,
        TransitionType::FlipVertical,
        TransitionType::IrisRound,
        TransitionType::IrisRoundClose,
        TransitionType::IrisBox,
        TransitionType::IrisBoxClose,
        TransitionType::BarnDoorHClose,
        TransitionType::BarnDoorVClose,
        TransitionType::ClockWipeCCW,
    };
    int curIdx = 0;
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        typeCombo->addItem(Transition::typeName(options[i]), static_cast<int>(options[i]));
        if (static_cast<int>(options[i]) == curType) curIdx = static_cast<int>(i);
    }
    typeCombo->setCurrentIndex(curIdx);

    auto *durSpin = new QDoubleSpinBox(&dialog);
    durSpin->setRange(0.1, 5.0);
    durSpin->setSingleStep(0.1);
    durSpin->setDecimals(2);
    durSpin->setSuffix(" s");
    durSpin->setValue(qBound(0.1, curDuration, 5.0));

    auto *easingCombo = new QComboBox(&dialog);
    const TransitionEasing easingOptions[] = {
        TransitionEasing::Linear,
        TransitionEasing::EaseIn,
        TransitionEasing::EaseOut,
        TransitionEasing::EaseInOut,
    };
    int curEasingIdx = 0;
    for (size_t i = 0; i < sizeof(easingOptions) / sizeof(easingOptions[0]); ++i) {
        easingCombo->addItem(Transition::easingName(easingOptions[i]),
            static_cast<int>(easingOptions[i]));
        if (static_cast<int>(easingOptions[i]) == curEasing)
            curEasingIdx = static_cast<int>(i);
    }
    easingCombo->setCurrentIndex(curEasingIdx);

    form->addRow("種類", typeCombo);
    form->addRow("時間", durSpin);
    form->addRow("イージング", easingCombo);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() == QDialog::Accepted) {
        prefs.setValue("transition/defaultType", typeCombo->currentData().toInt());
        prefs.setValue("transition/defaultDuration", durSpin->value());
        prefs.setValue("transition/defaultEasing", easingCombo->currentData().toInt());
        statusBar()->showMessage(
            QString("Default transition: %1 (%2s, %3)")
                .arg(typeCombo->currentText())
                .arg(durSpin->value(), 0, 'f', 1)
                .arg(easingCombo->currentText()),
            2000);
    }
}

void MainWindow::addImageOverlay()
{
    ImageOverlayDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        auto overlay = dialog.result();
        statusBar()->showMessage(QString("Added image: %1").arg(overlay.filePath));
    }
}

void MainWindow::addPip()
{
    int maxClip = m_timeline->videoClips().size() - 1;
    if (maxClip < 0) {
        QMessageBox::information(this, "PiP", "Add at least one clip first.");
        return;
    }
    PipDialog dialog(maxClip, this);
    if (dialog.exec() == QDialog::Accepted) {
        auto config = dialog.result();
        statusBar()->showMessage(QString("Added PiP from clip #%1").arg(config.sourceClipIndex));
    }
}

void MainWindow::colorCorrection()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Color Correction", "Select a clip first.");
        return;
    }

    // サブメニュー: 旧ダイアログ or 新パネル
    QMenu menu(this);
    auto *dialogAction = menu.addAction("色補正ダイアログ (クラシック)...");
    auto *panelAction  = menu.addAction("カラーグレーディングパネル");
    auto *chosen = menu.exec(QCursor::pos());
    if (!chosen) return;

    if (chosen == panelAction) {
        // 新パネルを表示
        m_colorGradingPanel->setColorCorrection(m_timeline->clipColorCorrection());
        m_colorGradingPanel->show();
        m_colorGradingPanel->raise();
    } else {
        // 旧ダイアログ (リアルタイムプレビュー付き)
        ColorCorrection originalCC = m_timeline->clipColorCorrection();
        ColorCorrectionDialog dialog(originalCC, this);

        // スライダー操作時にリアルタイムでプレビューに反映
        connect(&dialog, &ColorCorrectionDialog::colorCorrectionChanged,
                this, [this](const ColorCorrection &cc) {
            m_player->setColorCorrection(cc);
        });

        if (dialog.exec() == QDialog::Accepted) {
            // OK: 確定
            m_timeline->setClipColorCorrection(dialog.result());
            m_player->setColorCorrection(dialog.result());
            // パネルも同期
            m_colorGradingPanel->setColorCorrection(dialog.result());
            statusBar()->showMessage("色補正を適用しました");
        } else {
            // Cancel: 元に戻す
            m_player->setColorCorrection(originalCC);
        }
    }
}

void MainWindow::videoEffects()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Video Effects", "Select a clip first.");
        return;
    }
    const QVector<VideoEffect> original = m_timeline->clipEffects();
    VideoEffectDialog dialog(original, this);
    connect(&dialog, &VideoEffectDialog::effectsChanged, this,
            [this](const QVector<VideoEffect> &e) {
        if (m_player) m_player->setPreviewEffects(e);
    });
    if (m_player) m_player->setPreviewEffects(original);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (accepted) {
        m_timeline->setClipEffects(dialog.result());
        statusBar()->showMessage(QString("Applied %1 effect(s)").arg(dialog.result().size()));
    }
    if (m_player)
        m_player->setPreviewEffects(accepted ? dialog.result() : original, /*live=*/true);
}

void MainWindow::pluginEffects()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Plugin Effects", "Select a clip first.");
        return;
    }
    PluginEffectDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        auto plugin = PluginRegistry::instance().findByName(dialog.selectedPlugin());
        if (!plugin) return;

        // Apply plugin effect to the clip's current frame (stored as a custom effect)
        statusBar()->showMessage(QString("Applied plugin: %1").arg(plugin->name()));
    }
}

void MainWindow::editKeyframes()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Keyframes", "Select a clip first.");
        return;
    }

    // Let user choose which property to keyframe
    QStringList properties = {
        "colorCorrection.brightness", "colorCorrection.contrast",
        "colorCorrection.saturation", "colorCorrection.exposure",
        "colorCorrection.hue", "colorCorrection.temperature",
        "colorCorrection.gamma", "colorCorrection.highlights",
        "colorCorrection.shadows"
    };

    bool ok;
    QString prop = QInputDialog::getItem(this, "Edit Keyframes",
        "Select property to animate:", properties, 0, false, &ok);
    if (!ok) return;

    double clipDur = m_timeline->selectedClipDuration();
    auto km = m_timeline->clipKeyframes();

    // Get or create track for selected property
    KeyframeTrack track = km.hasTrack(prop) ?
        *km.track(prop) : KeyframeTrack(prop, 0.0);

    KeyframeDialog dialog(track, clipDur, this);
    if (dialog.exec() == QDialog::Accepted) {
        km.addTrack(dialog.result());
        m_timeline->setClipKeyframes(km);
        statusBar()->showMessage(QString("Keyframes updated: %1 (%2 points)")
            .arg(prop).arg(dialog.result().count()));
    }
}

void MainWindow::autoSilenceDetect()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Silence Detection", "Select a clip first.");
        return;
    }
    const auto &clips = m_timeline->videoClips();
    int sel = m_timeline->undoManager() ? 0 : 0; // use first clip if needed
    if (clips.isEmpty()) return;

    // Use selected clip's file
    int selIdx = 0;
    for (int i = 0; i < clips.size(); ++i) {
        // find selected
        if (i == m_timeline->videoClips().size() - 1 || m_timeline->hasSelection()) {
            selIdx = i; break;
        }
    }

    statusBar()->showMessage("Analyzing audio for silence...");
    auto silences = AutoEdit::detectSilenceFromFile(clips[selIdx].filePath);
    statusBar()->showMessage(QString("Found %1 silent region(s)").arg(silences.size()));

    if (silences.isEmpty()) {
        QMessageBox::information(this, "Silence Detection", "No significant silence detected.");
        return;
    }

    QString info;
    for (int i = 0; i < qMin(silences.size(), 20); ++i) {
        info += QString("%1. %2s - %3s (%4s)\n")
            .arg(i + 1)
            .arg(silences[i].startTime, 0, 'f', 1)
            .arg(silences[i].endTime, 0, 'f', 1)
            .arg(silences[i].duration(), 0, 'f', 1);
    }
    if (silences.size() > 20)
        info += QString("... and %1 more").arg(silences.size() - 20);

    QMessageBox::information(this, "Silence Detected", info);
}

void MainWindow::autoJumpCut()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Jump Cut", "Add clips first.");
        return;
    }

    auto reply = QMessageBox::question(this, "Auto Jump Cut",
        "This will automatically split clips at silent regions and remove the silence.\n\n"
        "Continue?", QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    statusBar()->showMessage("Analyzing and cutting...");
    const auto &clips = m_timeline->videoClips();

    int totalCuts = 0;
    for (const auto &clip : clips) {
        auto silences = AutoEdit::detectSilenceFromFile(clip.filePath);
        auto cuts = AutoEdit::generateJumpCuts(silences, clip.effectiveDuration());
        totalCuts += cuts.size() / 2;
    }

    statusBar()->showMessage(QString("Jump cut analysis complete: %1 cut point(s) found").arg(totalCuts));
    QMessageBox::information(this, "Jump Cut",
        QString("Found %1 potential cut(s).\n\n"
                "Use Edit > Split to manually apply cuts at detected points.")
            .arg(totalCuts));
}

void MainWindow::autoSceneDetect()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Scene Detection", "Add clips first.");
        return;
    }

    statusBar()->showMessage("Analyzing video for scene changes...");
    const auto &clip = m_timeline->videoClips().first();
    auto scenes = AutoEdit::detectSceneChanges(clip.filePath);
    statusBar()->showMessage(QString("Found %1 scene change(s)").arg(scenes.size()));

    if (scenes.isEmpty()) {
        QMessageBox::information(this, "Scene Detection", "No scene changes detected.");
        return;
    }

    QString info;
    for (int i = 0; i < qMin(scenes.size(), 30); ++i) {
        info += QString("%1. %2s (confidence: %3%)\n")
            .arg(i + 1)
            .arg(scenes[i].time, 0, 'f', 1)
            .arg(static_cast<int>(scenes[i].confidence * 100));
    }

    QMessageBox::information(this, "Scene Changes Detected", info);
}

void MainWindow::changeTheme()
{
    QStringList themes;
    for (const auto &t : ThemeManager::instance().availableThemes())
        themes << t.name;

    bool ok;
    QString selected = QInputDialog::getItem(this, "Change Theme",
        "Select theme:", themes, 0, false, &ok);
    if (!ok) return;

    ThemeType type = ThemeType::Dark;
    if (selected == "Light")    type = ThemeType::Light;
    else if (selected == "Midnight") type = ThemeType::Midnight;
    else if (selected == "Ocean")    type = ThemeType::Ocean;

    ThemeManager::instance().applyTheme(type, this);
    statusBar()->showMessage(QString("Theme: %1").arg(selected));
}

void MainWindow::multiCamSetup()
{
    if (!m_multiCam)
        m_multiCam = new MultiCamSession(this);

    // Add video files as camera sources
    QStringList files = QFileDialog::getOpenFileNames(this, "Add Camera Sources",
        QString(), "Video Files (*.mp4 *.mkv *.mov *.webm);;All Files (*)");

    for (const auto &file : files)
        m_multiCam->addSource(file);

    if (m_multiCam->sourceCount() < 2) {
        QMessageBox::information(this, "Multi-Camera",
            "Add at least 2 camera sources for multi-camera editing.");
        return;
    }

    auto reply = QMessageBox::question(this, "Multi-Camera Sync",
        QString("Added %1 camera source(s).\n\n"
                "Auto-sync cameras by audio?")
            .arg(m_multiCam->sourceCount()),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        statusBar()->showMessage("Syncing cameras by audio...");
        m_multiCam->autoSyncByAudio();
        QString info;
        for (int i = 0; i < m_multiCam->sourceCount(); ++i) {
            info += QString("%1: offset %2s\n")
                .arg(m_multiCam->sources()[i].label)
                .arg(m_multiCam->sources()[i].syncOffset, 0, 'f', 2);
        }
        statusBar()->showMessage("Sync complete");
        QMessageBox::information(this, "Sync Results", info);
    }
}

void MainWindow::multiCamSwitch()
{
    if (!m_multiCam || m_multiCam->sourceCount() < 2) {
        QMessageBox::information(this, "Multi-Camera", "Set up cameras first (Tools > Multi-Camera Setup).");
        return;
    }

    QStringList cameras;
    for (const auto &src : m_multiCam->sources())
        cameras << src.label;

    bool ok;
    QString selected = QInputDialog::getItem(this, "Switch Camera",
        "Select camera at current playhead:", cameras, 0, false, &ok);
    if (!ok) return;

    int idx = cameras.indexOf(selected);
    m_multiCam->switchToCamera(idx, m_timeline->playheadPosition());
    statusBar()->showMessage(QString("Switched to %1 at %2s")
        .arg(selected).arg(m_timeline->playheadPosition(), 0, 'f', 1));
}

// US-FEAT-D: motion tracking UI
void MainWindow::trackMotion()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Motion Tracking", "Select a clip first.");
        return;
    }

    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) return;
    const ClipInfo &clip = clips.first();

    if (!m_motionTracker)
        m_motionTracker = new MotionTracker(this);

    statusBar()->showMessage(
        QString::fromUtf8("\xE3\x83\x97\xE3\x83\xAC\xE3\x83\x93\xE3\x83\xA5"
                           "\xE3\x83\xBC\xE3\x81\xA7\xE3\x83\x88\xE3\x83\xA9"
                           "\xE3\x83\x83\xE3\x82\xAD\xE3\x83\xB3\xE3\x82\xB0"
                           "\xE9\xA0\x98\xE5\x9F\x9F\xE3\x82\x92\xE3\x83\x89"
                           "\xE3\x83\xA9\xE3\x83\x83\xE3\x82\xB0\xE3\x81\x97"
                           "\xE3\x81\xA6\xE9\x81\xB8\xE6\x8A\x9E\xE3\x81\x97"
                           "\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95"
                           "\xE3\x81\x84\xE3\x80\x82 (Esc \xE3\x81\xA7\xE3"
                           "\x82\xAD\xE3\x83\xA3\xE3\x83\xB3\xE3\x82\xBB\xE3"
                           "\x83\xAB)"), 0);

    // US-WIRE-3: replace hardcoded QRect(100,100,64,64) with a user-driven
    // rubber-band drag on the preview. The callback receives the rect in
    // source-frame coords and kicks off the tracking worker + progress UI.
    m_player->enterRegionPickerMode([this, clip](QRect region) {
        statusBar()->showMessage(QString());

        if (region.isNull() || region.width() <= 0 || region.height() <= 0) {
            // User cancelled (Esc or too-small drag)
            return;
        }

        QProgressDialog *progress = new QProgressDialog("Tracking motion\u2026", "Cancel", 0, 100, this);
        progress->setWindowTitle("Motion Tracking");
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(0);

        connect(m_motionTracker, &MotionTracker::progressChanged, progress, &QProgressDialog::setValue, Qt::UniqueConnection);

        connect(m_motionTracker, &MotionTracker::trackingComplete, this,
            [this, progress](const TrackingResult &result) {
                progress->close();
                progress->deleteLater();
                if (result.isEmpty()) {
                    QMessageBox::warning(this,
                        QString::fromUtf8("\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0"),
                        QString::fromUtf8("\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0\xe7\xb5\x90\xe6\x9e\x9c\xe3\x81\x8c\xe7\xa9\xba\xe3\x81\xa7\xe3\x81\x99"));
                    return;
                }

                const auto &vClips = m_timeline->videoClips();
                if (vClips.isEmpty()) return;
                const auto &mgr = vClips[0].textManager;
                const int overlayCount = mgr.count();

                if (overlayCount == 0) {
                    QMessageBox::information(this,
                        QString::fromUtf8("\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0\xe7\xb5\x90\xe6\x9e\x9c\xe3\x82\x92\xe9\x81\xa9\xe7\x94\xa8"),
                        QString::fromUtf8("\xe9\x81\xa9\xe7\x94\xa8\xe5\x85\x88\xe3\x81\xae\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x83\xac\xe3\x82\xa4\xe3\x81\x8c\xe3\x81\x82\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82\xe5\x85\x88\xe3\x81\xab\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x83\xac\xe3\x82\xa4\xe3\x82\x92\xe8\xbf\xbd\xe5\x8a\xa0\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84\xe3\x80\x82"));
                    return;
                }

                QDialog dlg(this);
                dlg.setWindowTitle(QString::fromUtf8("\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0\xe7\xb5\x90\xe6\x9e\x9c\xe3\x82\x92\xe9\x81\xa9\xe7\x94\xa8"));
                auto *layout = new QVBoxLayout(&dlg);

                auto *label = new QLabel(
                    QString("Tracking complete: %1 frames tracked.\n"
                            "\xe9\x81\xa9\xe7\x94\xa8\xe5\x85\x88\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x83\xac\xe3\x82\xa4\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84:")
                        .arg(result.regions.size()), &dlg);
                label->setWordWrap(true);
                layout->addWidget(label);

                auto *combo = new QComboBox(&dlg);
                for (int i = 0; i < overlayCount; ++i) {
                    const auto &ov = mgr.overlay(i);
                    combo->addItem(
                        QString("%1: \"%2\"")
                            .arg(i + 1)
                            .arg(ov.text.left(30)),
                        i);
                }
                layout->addWidget(combo);

                auto *btnBox = new QDialogButtonBox(
                    QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
                connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
                connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
                layout->addWidget(btnBox);

                if (dlg.exec() != QDialog::Accepted)
                    return;

                const int selIdx = combo->currentData().toInt();
                const auto &overlays = mgr.overlays();
                if (selIdx < 0 || selIdx >= overlays.size())
                    return;

                EnhancedTextOverlay selOverlay = overlays[selIdx];

                if (!selOverlay.positionKeyframes.isEmpty()) {
                    const int ret = QMessageBox::question(this,
                        QString::fromUtf8("\xe3\x82\xad\xe3\x83\xbc\xe3\x83\x95\xe3\x83\xac\xe3\x83\xbc\xe3\x83\xa0\xe7\xa2\xba\xe8\xaa\x8d"),
                        QString::fromUtf8("\xe3\x81\x93\xe3\x81\xae\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x83\xac\xe3\x82\xa4\xe3\x81\xab\xe3\x81\xaf\xe6\x97\xa2\xe5\xad\x98\xe3\x81\xae\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0\xe3\x82\xad\xe3\x83\xbc\xe3\x83\x95\xe3\x83\xac\xe3\x83\xbc\xe3\x83\xa0\xe3\x81\x8c\xe3\x81\x82\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82\n\xe6\x97\xa2\xe5\xad\x98\xe3\x81\xae\xe3\x82\xad\xe3\x83\xbc\xe3\x83\x95\xe3\x83\xac\xe3\x83\xbc\xe3\x83\xa0\xe3\x82\x92\xe4\xb8\x8a\xe6\x9b\xb8\xe3\x81\x8d\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x81\x8b\xef\xbc\x9f"),
                        QMessageBox::Yes | QMessageBox::No);
                    if (ret != QMessageBox::Yes)
                        return;
                }

                TrackerLink::applyToOverlay(&selOverlay,
                    result, m_projectConfig.fps);
                m_timeline->applyTrackingToOverlay(selIdx, selOverlay);

                {
                    const auto &updatedMgr = m_timeline->videoClips()[0].textManager;
                    QVector<EnhancedTextOverlay> ovList;
                    for (int i = 0; i < updatedMgr.count(); ++i)
                        ovList.append(updatedMgr.overlay(i));
                    m_player->setTextOverlays(ovList);
                }

                statusBar()->showMessage(
                    QString::fromUtf8("\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0\xe3\x82\x92\xe9\x81\xa9\xe7\x94\xa8\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f: %1\xe3\x83\x95\xe3\x83\xac\xe3\x83\xbc\xe3\x83\xa0\xe2\x86\x92\"%2\"")
                        .arg(selOverlay.positionKeyframes.size())
                        .arg(selOverlay.text.left(20)));
            }, Qt::UniqueConnection);

        connect(progress, &QProgressDialog::canceled, this, [this]() {
            if (m_motionTracker)
                m_motionTracker->cancel();
        });

        m_motionTracker->startTracking(clip.filePath, region);
        statusBar()->showMessage("Starting motion tracking\u2026");
    });
}

void MainWindow::motionTrackSetup()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Motion Tracking", "Select a clip first.");
        return;
    }

    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) return;

    // Let user define tracking region
    bool ok;
    int x = QInputDialog::getInt(this, "Motion Tracking", "Region X:", 100, 0, 9999, 1, &ok);
    if (!ok) return;
    int y = QInputDialog::getInt(this, "Motion Tracking", "Region Y:", 100, 0, 9999, 1, &ok);
    if (!ok) return;
    int w = QInputDialog::getInt(this, "Motion Tracking", "Region Width:", 64, 16, 512, 1, &ok);
    if (!ok) return;
    int h = QInputDialog::getInt(this, "Motion Tracking", "Region Height:", 64, 16, 512, 1, &ok);
    if (!ok) return;

    if (!m_motionTracker)
        m_motionTracker = new MotionTracker(this);

    connect(m_motionTracker, &MotionTracker::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Tracking... %1%").arg(pct));
    }, Qt::UniqueConnection);

    connect(m_motionTracker, &MotionTracker::trackingComplete, this, [this](const TrackingResult &result) {
        statusBar()->showMessage(QString("Tracking complete: %1 frames tracked")
            .arg(result.regions.size()));
        QMessageBox::information(this, "Motion Tracking",
            QString("Tracked %1 frames.\n\nUse Effects > Apply to Overlay to attach an overlay to the tracked path.")
                .arg(result.regions.size()));
    }, Qt::UniqueConnection);

    QRect region(x, y, w, h);
    m_motionTracker->startTracking(clips.first().filePath, region);
    statusBar()->showMessage("Starting motion tracking...");
}

void MainWindow::audioNoiseDenoise()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Audio Denoise", "Add clips first.");
        return;
    }

    const auto &clip = m_timeline->videoClips().first();

    bool ok;
    double reduction = QInputDialog::getDouble(this, "Audio Noise Reduction",
        "Noise reduction amount (0.0 = none, 1.0 = max):", 0.5, 0.0, 1.0, 2, &ok);
    if (!ok) return;

    double noiseFloor = QInputDialog::getDouble(this, "Audio Noise Reduction",
        "Noise floor (dB, -80 to 0):", -40.0, -80.0, 0.0, 0, &ok);
    if (!ok) return;

    QString outputPath = QFileDialog::getSaveFileName(this, "Save Denoised Audio",
        QFileInfo(clip.filePath).baseName() + "_denoised.wav",
        "Audio Files (*.wav *.mp3 *.aac);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_noiseReduction)
        m_noiseReduction = new NoiseReduction(this);

    AudioDenoiseConfig config;
    config.reductionAmount = reduction;
    config.noiseFloor = noiseFloor;

    connect(m_noiseReduction, &NoiseReduction::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Denoising audio... %1%").arg(pct));
    }, Qt::UniqueConnection);

    connect(m_noiseReduction, &NoiseReduction::denoiseComplete, this, [this](bool success, const QString &msg) {
        if (success)
            statusBar()->showMessage("Audio denoise complete: " + msg);
        else
            QMessageBox::warning(this, "Audio Denoise Failed", msg);
    }, Qt::UniqueConnection);

    m_noiseReduction->denoiseAudio(clip.filePath, outputPath, config);
    statusBar()->showMessage("Denoising audio...");
}

void MainWindow::videoNoiseDenoise()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Video Denoise", "Add clips first.");
        return;
    }

    const auto &clip = m_timeline->videoClips().first();

    QStringList methods = {"HQDN3D (Fast)", "NLMeans (High Quality)"};
    bool ok;
    QString method = QInputDialog::getItem(this, "Video Denoise",
        "Denoise method:", methods, 0, false, &ok);
    if (!ok) return;

    double spatial = QInputDialog::getDouble(this, "Video Denoise",
        "Spatial strength (0-30):", 4.0, 0.0, 30.0, 1, &ok);
    if (!ok) return;

    double temporal = QInputDialog::getDouble(this, "Video Denoise",
        "Temporal strength (0-30):", 6.0, 0.0, 30.0, 1, &ok);
    if (!ok) return;

    QString outputPath = QFileDialog::getSaveFileName(this, "Save Denoised Video",
        QFileInfo(clip.filePath).baseName() + "_denoised.mp4",
        "Video Files (*.mp4 *.mkv *.mov);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_noiseReduction)
        m_noiseReduction = new NoiseReduction(this);

    VideoDenoiseConfig config;
    config.spatialStrength = spatial;
    config.temporalStrength = temporal;
    config.method = method.startsWith("NLMeans") ? VideoDenoiseMethod::NLMeans : VideoDenoiseMethod::HQDN3D;

    connect(m_noiseReduction, &NoiseReduction::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Denoising video... %1%").arg(pct));
    }, Qt::UniqueConnection);

    connect(m_noiseReduction, &NoiseReduction::denoiseComplete, this, [this](bool success, const QString &msg) {
        if (success)
            statusBar()->showMessage("Video denoise complete: " + msg);
        else
            QMessageBox::warning(this, "Video Denoise Failed", msg);
    }, Qt::UniqueConnection);

    m_noiseReduction->denoiseVideo(clip.filePath, outputPath, config);
    statusBar()->showMessage("Denoising video...");
}

void MainWindow::generateSubtitles()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Subtitle Generation", "Add clips first.");
        return;
    }

    if (!SubtitleGenerator::isWhisperAvailable()) {
        QMessageBox::warning(this, "Whisper Not Found",
            "Whisper is not installed.\n\n"
            "Install with: pip install openai-whisper\n"
            "Or build whisper.cpp from source.");
        return;
    }

    const auto &clip = m_timeline->videoClips().first();

    QStringList languages = {"auto", "ja", "en", "zh", "ko", "fr", "de", "es", "it", "pt", "ru"};
    bool ok;
    QString lang = QInputDialog::getItem(this, "Subtitle Generation",
        "Language (auto = auto-detect):", languages, 0, false, &ok);
    if (!ok) return;

    if (!m_subtitleGen)
        m_subtitleGen = new SubtitleGenerator(this);

    WhisperConfig config;
    config.language = lang;

    connect(m_subtitleGen, &SubtitleGenerator::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Generating subtitles... %1%").arg(pct));
    }, Qt::UniqueConnection);

    connect(m_subtitleGen, &SubtitleGenerator::generationComplete, this, [this](const QVector<SubtitleSegment> &segments) {
        statusBar()->showMessage(QString("Generated %1 subtitle segment(s)").arg(segments.size()));

        if (segments.isEmpty()) {
            QMessageBox::information(this, "Subtitles", "No speech detected.");
            return;
        }

        // Ask to export
        QStringList options = {"Apply to clip as text overlays", "Export as SRT file", "Export as VTT file"};
        bool ok2;
        QString choice = QInputDialog::getItem(this, "Subtitles Generated",
            QString("%1 segments found. What to do?").arg(segments.size()),
            options, 0, false, &ok2);
        if (!ok2) return;

        if (choice.startsWith("Apply")) {
            auto overlays = m_subtitleGen->toTextOverlays(segments);
            auto clips = m_timeline->videoClips();
            if (!clips.isEmpty()) {
                for (const auto &o : overlays)
                    clips[0].textManager.addOverlay(o);
            }
            statusBar()->showMessage(QString("Applied %1 subtitle overlay(s)").arg(overlays.size()));
        } else {
            QString ext = choice.contains("SRT") ? "srt" : "vtt";
            QString filter = choice.contains("SRT") ? "SRT (*.srt)" : "WebVTT (*.vtt)";
            QString path = QFileDialog::getSaveFileName(this, "Export Subtitles",
                QString("subtitles.%1").arg(ext), filter);
            if (!path.isEmpty()) {
                bool exported = ext == "srt" ?
                    SubtitleGenerator::exportSRT(segments, path) :
                    SubtitleGenerator::exportVTT(segments, path);
                if (exported)
                    statusBar()->showMessage("Exported: " + path);
                else
                    QMessageBox::warning(this, "Export Failed", "Could not write subtitle file.");
            }
        }
    }, Qt::UniqueConnection);

    connect(m_subtitleGen, &SubtitleGenerator::errorOccurred, this, [this](const QString &error) {
        QMessageBox::warning(this, "Subtitle Generation Failed", error);
    }, Qt::UniqueConnection);

    m_subtitleGen->generate(clip.filePath, config);
    statusBar()->showMessage("Extracting audio and generating subtitles...");
}

void MainWindow::applyEffectPreset()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Effect Preset", "Select a clip first.");
        return;
    }

    auto &library = PresetLibrary::instance();
    QStringList presetNames;
    for (const auto &p : library.allPresets())
        presetNames << QString("%1 [%2]").arg(p.name, p.category);

    if (presetNames.isEmpty()) {
        QMessageBox::information(this, "Effect Preset", "No presets available.");
        return;
    }

    bool ok;
    QString selected = QInputDialog::getItem(this, "Apply Effect Preset",
        "Select preset:", presetNames, 0, false, &ok);
    if (!ok) return;

    // Extract name before the bracket
    QString name = selected.left(selected.lastIndexOf(" ["));
    auto result = library.applyPreset(name);

    m_timeline->setClipColorCorrection(result.first);
    m_timeline->setClipEffects(result.second);
    m_player->setColorCorrection(result.first);

    statusBar()->showMessage(QString("Applied preset: %1 (%2 effect(s))")
        .arg(name).arg(result.second.size()));
}

void MainWindow::saveEffectPreset()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Save Preset", "Select a clip first.");
        return;
    }

    bool ok;
    QString name = QInputDialog::getText(this, "Save Effect Preset",
        "Preset name:", QLineEdit::Normal, "My Preset", &ok);
    if (!ok || name.isEmpty()) return;

    QString category = QInputDialog::getText(this, "Save Effect Preset",
        "Category:", QLineEdit::Normal, "Custom", &ok);
    if (!ok) return;

    EffectPreset preset;
    preset.name = name;
    preset.category = category;
    preset.colorCorrection = m_timeline->clipColorCorrection();
    preset.effects = m_timeline->clipEffects();

    PresetLibrary::instance().addPreset(preset);
    PresetLibrary::instance().saveLibrary();
    statusBar()->showMessage(QString("Saved preset: %1").arg(name));
}

void MainWindow::manageEffectPresets()
{
    auto &library = PresetLibrary::instance();
    auto presets = library.allPresets();

    QString info = QString("Effect Presets (%1 total):\n\n").arg(presets.size());
    for (const auto &p : presets) {
        info += QString("• %1 [%2]%3\n")
            .arg(p.name, p.category)
            .arg(p.isBuiltIn ? " (built-in)" : "");
    }
    info += "\nUse Effects > Apply Preset to use, or Save Current as Preset to create new ones.";

    QMessageBox::information(this, "Manage Presets", info);
}

void MainWindow::stabilizeVideo()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Stabilize", "Add clips first.");
        return;
    }

    const auto &clip = m_timeline->videoClips().first();
    bool ok;
    int smoothing = QInputDialog::getInt(this, "Video Stabilization",
        "Smoothing (1-100, higher=smoother):", 10, 1, 100, 1, &ok);
    if (!ok) return;

    QString outputPath = QFileDialog::getSaveFileName(this, "Save Stabilized Video",
        QFileInfo(clip.filePath).baseName() + "_stabilized.mp4",
        "Video Files (*.mp4 *.mkv *.mov);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_stabilizer)
        m_stabilizer = new VideoStabilizer(this);

    StabilizerConfig config;
    config.smoothing = smoothing;

    connect(m_stabilizer, &VideoStabilizer::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Stabilizing... %1%").arg(pct));
    }, Qt::UniqueConnection);
    connect(m_stabilizer, &VideoStabilizer::stabilizeComplete, this, [this](bool ok2, const QString &msg) {
        statusBar()->showMessage(ok2 ? "Stabilization complete" : "Stabilization failed: " + msg);
    }, Qt::UniqueConnection);

    m_stabilizer->stabilize(clip.filePath, outputPath, config);
    statusBar()->showMessage("Stabilizing video (2-pass)...");
}

void MainWindow::applyLut()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "LUT", "Select a clip first.");
        return;
    }

    // Offer built-in or custom LUT
    QStringList options;
    for (const auto &lut : LutLibrary::instance().allLuts())
        options << lut.name;
    options << "Load .cube file...";

    bool ok;
    QString selected = QInputDialog::getItem(this, "Apply LUT",
        "Select LUT:", options, 0, false, &ok);
    if (!ok) return;

    LutData lut;
    if (selected == "Load .cube file...") {
        QString path = QFileDialog::getOpenFileName(this, "Load LUT",
            QString(), "Cube LUT (*.cube);;All Files (*)");
        if (path.isEmpty()) return;
        lut = LutImporter::loadCubeFile(path);
        if (!lut.isValid()) {
            QMessageBox::warning(this, "LUT Error", "Could not parse LUT file.");
            return;
        }
        LutLibrary::instance().addLut(path);
    } else {
        auto found = LutLibrary::instance().findByName(selected);
        if (!found.isValid()) return;
        lut = found;
    }

    double intensity = QInputDialog::getDouble(this, "LUT Intensity",
        "Intensity (0.0-1.0):", 1.0, 0.0, 1.0, 2, &ok);
    if (!ok) return;
    lut.intensity = intensity;

    statusBar()->showMessage(QString("Applied LUT: %1 (intensity %2)")
        .arg(lut.name).arg(intensity, 0, 'f', 1));
}

void MainWindow::loadLutCubeFile()
{
    QString path = QFileDialog::getOpenFileName(this, "LUT を読み込み",
        QString(), "Cube LUT (*.cube);;All Files (*)");
    if (path.isEmpty()) return;

    LutData lut = LutImporter::loadCubeFile(path);
    if (!lut.isValid()) {
        QMessageBox::warning(this, "LUT Error", "Could not parse LUT file.");
        return;
    }

    if (m_player)
        m_player->glPreview()->setLut(lut);

    if (m_lutIntensitySlider)
        m_lutIntensitySlider->setValue(100);

    LutLibrary::instance().addLut(path);
    if (m_colorGradingPanel)
        m_colorGradingPanel->setLutList(LutLibrary::instance().allLuts());

    statusBar()->showMessage(QString("LUT 読み込み: %1").arg(lut.name));
}

void MainWindow::clearLutIntensity()
{
    if (m_player)
        m_player->glPreview()->clearLut();
    if (m_lutIntensitySlider)
        m_lutIntensitySlider->setValue(0);
    statusBar()->showMessage("LUT 解除");
}

void MainWindow::manageLuts()
{
    auto &library = LutLibrary::instance();
    auto luts = library.allLuts();

    QString info = QString("LUT Library (%1 total):\n\n").arg(luts.size());
    for (const auto &l : luts) {
        info += QString("• %1 [%2x%2x%2]%3\n")
            .arg(l.name).arg(l.size)
            .arg(l.isBuiltIn() ? " (built-in)" : "");
    }
    QMessageBox::information(this, "Manage LUTs", info);
}

void MainWindow::openProxySettings()
{
    auto &pm = ProxyManager::instance();

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("プロキシ設定"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *modeCheck = new QCheckBox(
        QStringLiteral("プロキシ再生 (低解像度ファイルで再生)"), &dlg);
    modeCheck->setChecked(pm.isProxyMode());
    modeCheck->setToolTip(QStringLiteral(
        "ON: 生成済みプロキシをタイムラインで使用 (高速再生)\n"
        "OFF: 元解像度ファイルを使用"));
    layout->addWidget(modeCheck);

    // Encoder override (US-1): empty itemData = Auto. Probe each GPU
    // encoder up-front and disable items the runtime ffmpeg can't run so
    // the user can't pin an encoder that will fall through to libx264.
    layout->addWidget(new QLabel(QStringLiteral("エンコーダー:"), &dlg));
    auto *encoderCombo = new QComboBox(&dlg);
    struct EncOpt { const char *label; const char *value; };
    const EncOpt encOpts[] = {
        {"Auto (自動検出)",      ""},
        {"NVIDIA NVENC",         "h264_nvenc"},
        {"Intel QSV",            "h264_qsv"},
        {"AMD AMF",              "h264_amf"},
        {"CPU (libx264)",        "libx264"},
    };
    auto *encModel = new QStandardItemModel(encoderCombo);
    for (const auto &opt : encOpts) {
        auto *item = new QStandardItem(QString::fromUtf8(opt.label));
        item->setData(QString::fromLatin1(opt.value), Qt::UserRole);
        const QString enc = QString::fromLatin1(opt.value);
        const bool isGpu = (enc == "h264_nvenc" || enc == "h264_qsv" || enc == "h264_amf");
        if (isGpu && !ProxyManager::ffmpegHasEncoder(enc)) {
            item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
        }
        encModel->appendRow(item);
    }
    encoderCombo->setModel(encModel);
    const QString currentEnc = pm.encoderOverride();
    for (int i = 0; i < encoderCombo->count(); ++i) {
        if (encoderCombo->itemData(i, Qt::UserRole).toString() == currentEnc) {
            encoderCombo->setCurrentIndex(i);
            break;
        }
    }
    layout->addWidget(encoderCombo);

    // Quality preset (US-2). Index 0..2 maps to QualityPreset enum directly.
    layout->addWidget(new QLabel(QStringLiteral("品質:"), &dlg));
    auto *qualityCombo = new QComboBox(&dlg);
    qualityCombo->addItem(QStringLiteral("High"),   static_cast<int>(QualityPreset::High));
    qualityCombo->addItem(QStringLiteral("Medium"), static_cast<int>(QualityPreset::Medium));
    qualityCombo->addItem(QStringLiteral("Low"),    static_cast<int>(QualityPreset::Low));
    {
        const int cur = static_cast<int>(pm.qualityPreset());
        for (int i = 0; i < qualityCombo->count(); ++i) {
            if (qualityCombo->itemData(i).toInt() == cur) {
                qualityCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    layout->addWidget(qualityCombo);

    // Storage directory (US-3). Read-only QLineEdit shows the resolved path
    // (custom QSettings value or default). 'フォルダ選択...' opens a dir
    // picker; we validate write-ability before persisting.
    layout->addWidget(new QLabel(QStringLiteral("保存先:"), &dlg));
    auto *storageRow = new QHBoxLayout();
    auto *storageEdit = new QLineEdit(&dlg);
    storageEdit->setReadOnly(true);
    storageEdit->setText(ProxyManager::proxyDir());
    auto *storageBtn = new QPushButton(QStringLiteral("フォルダ選択..."), &dlg);
    storageRow->addWidget(storageEdit);
    storageRow->addWidget(storageBtn);
    layout->addLayout(storageRow);
    auto *storageNote = new QLabel(
        QStringLiteral("既存 proxy は元の場所に残ります"), &dlg);
    storageNote->setStyleSheet("color:#888; font-size:10px;");
    layout->addWidget(storageNote);
    QString pendingStorage; // empty = no change
    connect(storageBtn, &QPushButton::clicked, &dlg, [&dlg, storageEdit, &pendingStorage]() {
        const QString picked = QFileDialog::getExistingDirectory(
            &dlg,
            QStringLiteral("プロキシ保存先を選択"),
            storageEdit->text());
        if (picked.isEmpty())
            return;
        QFileInfo info(picked);
        if (!info.isDir() || !info.isWritable()) {
            QMessageBox::warning(&dlg, QStringLiteral("プロキシ保存先"),
                QStringLiteral("選択したフォルダに書き込めません。\n別のフォルダを選択してください。"));
            return;
        }
        pendingStorage = picked;
        storageEdit->setText(picked);
    });

    auto *divisorLabel = new QLabel(
        QStringLiteral("プレビュー解像度 (CPU エフェクト適用時に効く):"), &dlg);
    layout->addWidget(divisorLabel);
    auto *divisorCombo = new QComboBox(&dlg);
    divisorCombo->addItem(QStringLiteral("Full (1/1)"), 1);
    divisorCombo->addItem(QStringLiteral("Half (1/2)"), 2);
    divisorCombo->addItem(QStringLiteral("Quarter (1/4)"), 4);
    divisorCombo->addItem(QStringLiteral("Eighth (1/8)"), 8);
    const int currentDivisor = m_player ? m_player->proxyDivisor() : 1;
    for (int i = 0; i < divisorCombo->count(); ++i) {
        if (divisorCombo->itemData(i).toInt() == currentDivisor) {
            divisorCombo->setCurrentIndex(i);
            break;
        }
    }
    layout->addWidget(divisorCombo);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    pm.setEncoderOverride(encoderCombo->currentData(Qt::UserRole).toString());
    pm.setQualityPreset(static_cast<QualityPreset>(qualityCombo->currentData().toInt()));
    if (!pendingStorage.isEmpty()) {
        ProxyManager::setProxyStorageDir(pendingStorage);
        QDir().mkpath(pendingStorage);
    }

    // Apply proxy mode flip first — the refresh below relies on the new
    // mode to resolve paths correctly.
    const bool newMode = modeCheck->isChecked();
    if (pm.isProxyMode() != newMode) {
        pm.setProxyMode(newMode);
        statusBar()->showMessage(newMode
            ? QStringLiteral("Proxy mode ON (low-res playback)")
            : QStringLiteral("Proxy mode OFF (original quality)"));
        if (m_timeline && m_player) {
            const bool wasPlaying = m_player->isPlaying();
            const int64_t posUs = m_player->timelinePositionUs();
            if (wasPlaying)
                m_player->pause();
            m_timeline->refreshPlaybackSequence();
            m_player->seek(static_cast<int>(posUs / 1000));
            if (wasPlaying)
                m_player->play();
        }
    }
    if (m_player)
        m_player->setProxyDivisor(divisorCombo->currentData().toInt());
}

void MainWindow::toggleProxyMode()
{
    auto &pm = ProxyManager::instance();
    pm.setProxyMode(!pm.isProxyMode());
    statusBar()->showMessage(pm.isProxyMode() ? "Proxy mode ON (low-res playback)" : "Proxy mode OFF (original quality)");

    if (!m_timeline || !m_player)
        return;
    // Snapshot playback state, refresh the sequence so getProxyPath picks
    // up the new resolution, then re-seek to where we were. setSequence's
    // own clamped restoration is not enough because loadFile resets the
    // file-local position to 0 and the decoder bootstraps from there —
    // the explicit seek after refresh forces the new decoder to land on
    // the same timeline position the user was watching.
    const bool wasPlaying = m_player->isPlaying();
    const int64_t posUs = m_player->timelinePositionUs();
    if (wasPlaying)
        m_player->pause();
    m_timeline->refreshPlaybackSequence();
    m_player->seek(static_cast<int>(posUs / 1000));
    if (wasPlaying)
        m_player->play();
}

void MainWindow::generateProxies()
{
    if (!m_timeline) {
        QMessageBox::information(this, "Proxies", "Timeline not ready.");
        return;
    }
    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) {
        QMessageBox::information(this, "Proxies", "Add clips first.");
        return;
    }

    QStringList paths;
    for (const auto &c : clips)
        paths << c.filePath;

    auto &pm = ProxyManager::instance();

    // Drop any prior allProxiesReady / progressChanged lambdas before
    // wiring up the new ones. Qt::UniqueConnection does NOT deduplicate
    // distinct lambda objects (each closure is a separate functor type),
    // so without this disconnect the Nth generateProxies call fires the
    // refresh handler N times on completion — the visible symptom is the
    // preview snapping back to position multiple times in a row.
    disconnect(&pm, &ProxyManager::allProxiesReady, this, nullptr);
    disconnect(&pm, &ProxyManager::progressChanged, this, nullptr);

    // generateAllProxies skips clips whose entry is Ready/Generating, so a
    // bare invocation on a project that already has proxies would emit
    // allProxiesReady immediately without doing any work — confusing if the
    // user expected to "regenerate". Offer to delete the existing proxies
    // first so the queue actually runs.
    int existing = 0;
    for (const auto &p : paths)
        if (pm.hasProxy(p)) ++existing;
    if (existing > 0) {
        const auto reply = QMessageBox::question(
            this, "Proxies",
            QString("既存のプロキシ %1 個を削除して再生成しますか?\n\n"
                    "「いいえ」を選ぶと既存プロキシをそのまま使い、未生成のクリップだけ生成します。")
                .arg(existing),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
            QMessageBox::No);
        if (reply == QMessageBox::Cancel)
            return;
        if (reply == QMessageBox::Yes) {
            // Releasing the proxy file while VideoPlayer is mid-decode on
            // it crashes the app — the avformat demuxer holds an open
            // handle that suddenly points at deleted bytes. Cancel any
            // in-flight ffmpeg job, force the player off proxy paths, and
            // only then unlink the files. The proxy mode flag is restored
            // afterwards so the new generation will switch the preview to
            // the freshly-built proxies once allProxiesReady fires.
            pm.cancelGeneration();
            if (m_player && m_player->isPlaying())
                m_player->pause();
            const bool wasProxyMode = pm.isProxyMode();
            if (wasProxyMode) {
                pm.setProxyMode(false);
                if (m_timeline)
                    m_timeline->refreshPlaybackSequence();
            }
            for (const auto &p : paths)
                pm.deleteProxy(p);
            if (wasProxyMode)
                pm.setProxyMode(true);
        }
    }

    connect(&pm, &ProxyManager::allProxiesReady, this, [this]() {
        // Qt::SingleShotConnection so the lambda is removed automatically
        // after one fire, in addition to the upfront disconnect — defense
        // in depth against the same lambda accumulating across calls.
        statusBar()->showMessage("All proxies generated");
        if (!m_timeline || !m_player)
            return;
        // VideoPlayer is sequence-driven (Timeline::sequenceChanged →
        // MainWindow's setSequence lambda → m_player->setSequence). A bare
        // loadFile() does not retarget the active sequence, so the preview
        // would keep playing the original even after generation finishes.
        // Re-emit the sequences so getProxyPath picks up the now-Ready paths.
        //
        // Pause/play wrap + explicit re-seek matches toggleProxyMode: the
        // file swap inside setSequence resets the decoder's file-local
        // position to 0, so we have to push the timeline position back
        // ourselves once the new entries are in place.
        const bool wasPlaying = m_player->isPlaying();
        const int64_t posUs = m_player->timelinePositionUs();
        if (wasPlaying)
            m_player->pause();
        m_timeline->refreshPlaybackSequence();
        m_player->seek(static_cast<int>(posUs / 1000));
        if (wasPlaying)
            m_player->play();
    }, Qt::SingleShotConnection);
    connect(&pm, &ProxyManager::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Generating proxies... %1%").arg(pct));
    });

    pm.generateAllProxies(paths);
    statusBar()->showMessage("Generating proxy files...");
}

void MainWindow::openLoudnessSettings()
{
    QSettings prefs("VSimpleEditor", "Preferences");
    const double initAmount =
        prefs.value("audio/normalizerAmount", 0.0).toDouble();
    const double initUniformity =
        prefs.value("audio/normalizerUniformity", 0.5).toDouble();

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("オーディオ均一化"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *intro = new QLabel(QStringLiteral(
        "<b>全トラックの出力レベルを動的に均一化します。</b><br>"
        "<small>"
        "適用量 0% で完全 OFF。均一性が高いほど反応が速く出力が平らになり、"
        "低いほど元の強弱が残ります。"
        "</small>"), &dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // Amount slider 0..100 == 0..1.0
    auto *amountRow = new QHBoxLayout();
    amountRow->addWidget(new QLabel(QStringLiteral("適用量 (Amount):"), &dlg));
    auto *amountValue = new QLabel(&dlg);
    amountValue->setMinimumWidth(48);
    amountValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    amountRow->addWidget(amountValue);
    layout->addLayout(amountRow);
    auto *amountSlider = new QSlider(Qt::Horizontal, &dlg);
    amountSlider->setRange(0, 100);
    amountSlider->setValue(static_cast<int>(qBound(0.0, initAmount, 1.0) * 100.0));
    amountValue->setText(QString::number(amountSlider->value()) + " %");
    layout->addWidget(amountSlider);

    auto *uniformRow = new QHBoxLayout();
    uniformRow->addWidget(new QLabel(QStringLiteral("均一性 (Uniformity):"), &dlg));
    auto *uniformValue = new QLabel(&dlg);
    uniformValue->setMinimumWidth(48);
    uniformValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    uniformRow->addWidget(uniformValue);
    layout->addLayout(uniformRow);
    auto *uniformSlider = new QSlider(Qt::Horizontal, &dlg);
    uniformSlider->setRange(0, 100);
    uniformSlider->setValue(static_cast<int>(qBound(0.0, initUniformity, 1.0) * 100.0));
    uniformValue->setText(QString::number(uniformSlider->value()) + " %");
    layout->addWidget(uniformSlider);

    AudioMixer *mixer = m_player ? m_player->audioMixer() : nullptr;

    auto pushAmount = [mixer, amountValue](int v) {
        amountValue->setText(QString::number(v) + " %");
        if (mixer) mixer->setNormalizerAmount(v / 100.0);
    };
    auto pushUniformity = [mixer, uniformValue](int v) {
        uniformValue->setText(QString::number(v) + " %");
        if (mixer) mixer->setNormalizerUniformity(v / 100.0);
    };
    connect(amountSlider, &QSlider::valueChanged, &dlg, pushAmount);
    connect(uniformSlider, &QSlider::valueChanged, &dlg, pushUniformity);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        const double amount = amountSlider->value() / 100.0;
        const double uniformity = uniformSlider->value() / 100.0;
        prefs.setValue("audio/normalizerAmount", amount);
        prefs.setValue("audio/normalizerUniformity", uniformity);
        if (mixer) {
            mixer->setNormalizerAmount(amount);
            mixer->setNormalizerUniformity(uniformity);
        }
    } else {
        // Cancel restores the original values (they were live-applied during
        // slider drag).
        if (mixer) {
            mixer->setNormalizerAmount(initAmount);
            mixer->setNormalizerUniformity(initUniformity);
        }
    }
}

void MainWindow::openProxyManagement()
{
    ProxyManagementDialog dlg(this);
    dlg.exec();
}

void MainWindow::setSpeedRamp()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Speed Ramp", "Select a clip first.");
        return;
    }

    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) return;

    bool ok;
    double startSpeed = QInputDialog::getDouble(this, "Speed Ramp",
        "Start speed (0.1-10x):", 1.0, 0.1, 10.0, 1, &ok);
    if (!ok) return;
    double endSpeed = QInputDialog::getDouble(this, "Speed Ramp",
        "End speed (0.1-10x):", 2.0, 0.1, 10.0, 1, &ok);
    if (!ok) return;

    QStringList easings = {"Linear", "Ease In", "Ease Out", "Ease In/Out"};
    QString easing = QInputDialog::getItem(this, "Speed Ramp",
        "Easing:", easings, 3, false, &ok);
    if (!ok) return;

    QString outputPath = QFileDialog::getSaveFileName(this, "Save Speed Ramp Video",
        QFileInfo(clips.first().filePath).baseName() + "_speedramp.mp4",
        "Video Files (*.mp4 *.mkv *.mov);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_speedRamp)
        m_speedRamp = new SpeedRamp(this);

    SpeedEasing e = SpeedEasing::Linear;
    if (easing.contains("In/Out")) e = SpeedEasing::EaseInOut;
    else if (easing.contains("In"))  e = SpeedEasing::EaseIn;
    else if (easing.contains("Out")) e = SpeedEasing::EaseOut;

    m_speedRamp->clearPoints();
    m_speedRamp->addPoint(0.0, startSpeed, SpeedEasing::Linear);
    m_speedRamp->addPoint(clips.first().effectiveDuration(), endSpeed, e);

    SpeedRampConfig config;
    config.points = {SpeedPoint{0.0, startSpeed, SpeedEasing::Linear},
                     SpeedPoint{clips.first().effectiveDuration(), endSpeed, e}};

    connect(m_speedRamp, &SpeedRamp::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Applying speed ramp... %1%").arg(pct));
    }, Qt::UniqueConnection);
    connect(m_speedRamp, &SpeedRamp::rampComplete, this, [this](bool ok2, const QString &msg) {
        statusBar()->showMessage(ok2 ? "Speed ramp complete" : "Speed ramp failed: " + msg);
    }, Qt::UniqueConnection);

    m_speedRamp->applySpeedRamp(clips.first().filePath, outputPath, config);
}

void MainWindow::audioEqualizer()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Equalizer", "Add clips first.");
        return;
    }

    auto presets = AudioEQProcessor::presets();
    QStringList presetNames;
    for (const auto &p : presets)
        presetNames << p.name;

    bool ok;
    QString selected = QInputDialog::getItem(this, "Audio Equalizer",
        "Select EQ preset:", presetNames, 0, false, &ok);
    if (!ok) return;

    const auto &clip = m_timeline->videoClips().first();
    QString outputPath = QFileDialog::getSaveFileName(this, "Save EQ Audio",
        QFileInfo(clip.filePath).baseName() + "_eq.mp4",
        "Media Files (*.mp4 *.wav *.mp3);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_audioEQ)
        m_audioEQ = new AudioEQProcessor(this);

    AudioEQConfig eqConfig;
    for (const auto &p : presets) {
        if (p.name == selected) { eqConfig = p.config; break; }
    }

    connect(m_audioEQ, &AudioEQProcessor::processComplete, this, [this](bool ok2, const QString &msg) {
        statusBar()->showMessage(ok2 ? "EQ applied: " + msg : "EQ failed: " + msg);
    }, Qt::UniqueConnection);

    m_audioEQ->applyEQ(clip.filePath, outputPath, eqConfig);
    statusBar()->showMessage("Applying EQ: " + selected);
}

void MainWindow::audioEffects()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Audio Effects", "Add clips first.");
        return;
    }

    QStringList effects = {"Reverb", "Compressor", "Normalize", "Fade In", "Fade Out", "Bass Boost", "Voice Enhance"};
    bool ok;
    QString selected = QInputDialog::getItem(this, "Audio Effects",
        "Select effect:", effects, 0, false, &ok);
    if (!ok) return;

    const auto &clip = m_timeline->videoClips().first();
    QString outputPath = QFileDialog::getSaveFileName(this, "Save Audio Effect",
        QFileInfo(clip.filePath).baseName() + "_fx.mp4",
        "Media Files (*.mp4 *.wav *.mp3);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_audioEQ)
        m_audioEQ = new AudioEQProcessor(this);

    AudioEffect effect;
    if (selected == "Reverb")         effect = AudioEffect::createReverb();
    else if (selected == "Compressor")effect = AudioEffect::createCompressor();
    else if (selected == "Normalize") effect = AudioEffect::createNormalize();
    else if (selected == "Fade In")   effect = AudioEffect::createFadeIn();
    else if (selected == "Fade Out")  effect = AudioEffect::createFadeOut();
    else if (selected == "Bass Boost")effect = AudioEffect::createBassBoost();
    else                              effect = AudioEffect::createVoiceEnhance();

    connect(m_audioEQ, &AudioEQProcessor::processComplete, this, [this](bool ok2, const QString &msg) {
        statusBar()->showMessage(ok2 ? "Effect applied: " + msg : "Effect failed: " + msg);
    }, Qt::UniqueConnection);

    m_audioEQ->applyEffect(clip.filePath, outputPath, effect);
    statusBar()->showMessage("Applying: " + selected);
}

void MainWindow::addMarker()
{
    double time = m_timeline->playheadPosition();
    bool ok;
    QString name = QInputDialog::getText(this, "Add Marker",
        QString("Marker name (at %1s):").arg(time, 0, 'f', 1),
        QLineEdit::Normal, "", &ok);
    if (!ok) return;

    QStringList types = {"Standard", "Chapter", "Todo", "Note"};
    QString typeStr = QInputDialog::getItem(this, "Marker Type",
        "Type:", types, 0, false, &ok);
    if (!ok) return;

    MarkerType type = MarkerType::Standard;
    if (typeStr == "Chapter") type = MarkerType::Chapter;
    else if (typeStr == "Todo") type = MarkerType::Todo;
    else if (typeStr == "Note") type = MarkerType::Note;

    m_markerManager.addMarker(time, name, type);
    statusBar()->showMessage(QString("Marker added: \"%1\" at %2s").arg(name).arg(time, 0, 'f', 1));
}

void MainWindow::showMarkers()
{
    auto markers = m_markerManager.markers();
    if (markers.isEmpty()) {
        QMessageBox::information(this, "Markers", "No markers set. Use Ctrl+M to add markers.");
        return;
    }

    QString info = QString("Markers (%1 total):\n\n").arg(markers.size());
    for (const auto &m : markers) {
        info += QString("• [%1s] %2 (%3)\n")
            .arg(m.time, 0, 'f', 1).arg(m.name)
            .arg(m.type == MarkerType::Chapter ? "Chapter" :
                 m.type == MarkerType::Todo ? "Todo" :
                 m.type == MarkerType::Note ? "Note" : "Standard");
    }
    QMessageBox::information(this, "Timeline Markers", info);
}

void MainWindow::exportChapters()
{
    auto chapters = m_markerManager.exportYouTubeChapters();
    if (chapters.isEmpty()) {
        QMessageBox::information(this, "Chapters", "No chapter markers found. Add markers with type 'Chapter' first.");
        return;
    }

    QString path = QFileDialog::getSaveFileName(this, "Export Chapters",
        "chapters.txt", "Text Files (*.txt);;All Files (*)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(chapters.toUtf8());
        file.close();
        statusBar()->showMessage("Exported chapters: " + path);
    }
}

void MainWindow::openRenderQueue()
{
    if (!m_renderQueue)
        m_renderQueue = new RenderQueue(this);

    auto jobs = m_renderQueue->jobs();
    QString info = QString("Render Queue (%1 jobs):\n\n").arg(jobs.size());
    for (const auto &j : jobs) {
        QString status = j.status == RenderJobStatus::Pending ? "Pending" :
                         j.status == RenderJobStatus::Rendering ? "Rendering" :
                         j.status == RenderJobStatus::Completed ? "Done" :
                         j.status == RenderJobStatus::Failed ? "Failed" : "Cancelled";
        info += QString("• %1 — %2 (%3%)\n").arg(j.name, status).arg(j.progress);
    }
    if (jobs.isEmpty())
        info += "Queue is empty. Export videos to add them to the queue.";

    QMessageBox::information(this, "Render Queue", info);
}

void MainWindow::startScreenRecording()
{
    if (!m_screenRecorder)
        m_screenRecorder = new ScreenRecorder(this);

    if (m_screenRecorder->isRecording()) {
        QMessageBox::information(this, "Recording", "Already recording. Stop first.");
        return;
    }

    RecordingConfig config;
    config.outputPath = QFileDialog::getSaveFileName(this, "Save Recording",
        "screen_recording.mp4", "Video Files (*.mp4 *.mkv);;All Files (*)");
    if (config.outputPath.isEmpty()) return;

    connect(m_screenRecorder, &ScreenRecorder::durationChanged, this, [this](double sec) {
        statusBar()->showMessage(QString("Recording: %1s").arg(sec, 0, 'f', 0));
    }, Qt::UniqueConnection);
    connect(m_screenRecorder, &ScreenRecorder::recordingStopped, this, [this](const QString &path) {
        statusBar()->showMessage("Recording saved: " + path);
    }, Qt::UniqueConnection);
    connect(m_screenRecorder, &ScreenRecorder::recordingError, this, [this](const QString &err) {
        QMessageBox::warning(this, "Recording Error", err);
    }, Qt::UniqueConnection);

    m_screenRecorder->startRecording(config);
    statusBar()->showMessage("Screen recording started...");
}

void MainWindow::stopScreenRecording()
{
    if (!m_screenRecorder || !m_screenRecorder->isRecording()) {
        QMessageBox::information(this, "Recording", "Not currently recording.");
        return;
    }
    m_screenRecorder->stopRecording();
}

void MainWindow::analyzeHighlights()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "AI Highlights", "Add clips first.");
        return;
    }

    const auto &clip = m_timeline->videoClips().first();

    bool ok;
    int count = QInputDialog::getInt(this, "AI Auto-Highlight",
        "Number of highlights to find:", 10, 1, 50, 1, &ok);
    if (!ok) return;

    if (!m_aiHighlight)
        m_aiHighlight = new AIHighlight(this);

    HighlightConfig config;
    config.targetCount = count;

    connect(m_aiHighlight, &AIHighlight::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Analyzing highlights... %1%").arg(pct));
    }, Qt::UniqueConnection);

    connect(m_aiHighlight, &AIHighlight::analysisComplete, this, [this](const QVector<Highlight> &highlights) {
        if (highlights.isEmpty()) {
            QMessageBox::information(this, "Highlights", "No highlights found.");
            return;
        }

        QString info = QString("Found %1 highlight(s):\n\n").arg(highlights.size());
        for (int i = 0; i < highlights.size(); ++i) {
            const auto &h = highlights[i];
            info += QString("#%1  %2s - %3s  [score: %4]\n")
                .arg(i + 1).arg(h.startTime, 0, 'f', 1)
                .arg(h.endTime, 0, 'f', 1).arg(h.score, 0, 'f', 2);
        }

        auto reply = QMessageBox::question(this, "AI Highlights", info + "\nExport highlight reel?");
        if (reply == QMessageBox::Yes) {
            QString outputPath = QFileDialog::getSaveFileName(this, "Save Highlight Reel",
                "highlights.mp4", "Video Files (*.mp4);;All Files (*)");
            if (!outputPath.isEmpty()) {
                const auto &clip2 = m_timeline->videoClips().first();
                m_aiHighlight->exportHighlightReel(clip2.filePath, outputPath, highlights);
                statusBar()->showMessage("Exporting highlight reel...");
            }
        }
    }, Qt::UniqueConnection);

    m_aiHighlight->analyze(clip.filePath, config);
    statusBar()->showMessage("Analyzing video for highlights...");
}

void MainWindow::addShapeLayer()
{
    QStringList shapes = {"Rectangle", "Rounded Rectangle", "Ellipse", "Polygon", "Star", "Line", "Arrow"};
    bool ok;
    QString selected = QInputDialog::getItem(this, "Add Shape Layer",
        "Shape type:", shapes, 0, false, &ok);
    if (!ok) return;

    ShapeLayer shapeLayer;
    ShapeFill fill;
    fill.color = QColor(65, 105, 225); // Royal blue
    fill.enabled = true;
    ShapeStroke stroke;
    stroke.color = Qt::white;
    stroke.width = 2.0;
    stroke.enabled = true;

    if (selected == "Star") {
        shapeLayer.addShape(ShapeLayer::createStar(5, 80, 40, fill, stroke));
    } else if (selected == "Ellipse") {
        shapeLayer.addShape(ShapeLayer::createCircle(60, fill, stroke));
    } else {
        shapeLayer.addShape(ShapeLayer::createRectangle(QSizeF(200, 120), fill, stroke));
    }

    statusBar()->showMessage(QString("Added shape layer: %1").arg(selected));
}

void MainWindow::addParticleEffect()
{
    auto presets = ParticleSystem::presetConfigs();
    QStringList presetNames = presets.keys();

    bool ok;
    QString selected = QInputDialog::getItem(this, "Add Particle Effect",
        "Particle preset:", presetNames, 0, false, &ok);
    if (!ok) return;

    if (presets.contains(selected)) {
        ParticleSystem ps;
        ps.setConfig(presets.value(selected));
        statusBar()->showMessage(QString("Added particle effect: %1").arg(selected));
    }
}

void MainWindow::addTextAnimation()
{
    bool ok;
    QString text = QInputDialog::getText(this, "Text Animation",
        "Text to animate:", QLineEdit::Normal, "Hello!", &ok);
    if (!ok || text.isEmpty()) return;

    auto presets = TextAnimator::presetAnimations();
    QStringList animNames = presets.keys();

    QString selected = QInputDialog::getItem(this, "Text Animation",
        "Animation style:", animNames, 0, false, &ok);
    if (!ok) return;

    if (presets.contains(selected)) {
        TextAnimator animator;
        animator.setText(text, QFont("Arial", 48), QPointF(100, 100));
        animator.setAnimation(presets.value(selected));
        statusBar()->showMessage(QString("Added text animation: \"%1\" with %2")
            .arg(text, selected));
    }
}

void MainWindow::addBrushAnimation()
{
    BrushAnimationDialog dialog(this);
    if (dialog.exec() == QDialog::Rejected)
        return;

    auto params = dialog.params();

    auto brushAnim = new BrushAnimation(this);
    brushAnim->setText(params.text, params.font, params.basePosition);
    brushAnim->setBrushWidth(params.brushWidth);
    brushAnim->setMode(params.mode);

    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) {
        statusBar()->showMessage("ブラシアニメ追加失敗 — クリップが見つかりません");
        brushAnim->deleteLater();
        return;
    }

    // AC2 — attach to the first selected video clip (fallback: first clip on
    // first video track), mirroring addTextOverlayToFirstVideoClip pattern.
    const int trackIdx = 0;
    const int selectedIdx = m_timeline->selectedVideoClipIndex();
    const int clipIdx = (selectedIdx >= 0 && selectedIdx < clips.size()) ? selectedIdx : 0;
    const QString clipId = brushClipId(trackIdx, clipIdx);

    BrushAnimationEntry entry;
    entry.clipId = clipId;
    entry.brushData = brushAnim->toJson();
    upsertBrushAnimationEntry(entry);

    if (auto *previous = m_liveBrushAnimations.value(clipId, nullptr))
        previous->deleteLater();
    m_liveBrushAnimations.insert(clipId, brushAnim);

    KeyframeManager km = m_timeline->clipKeyframes();
    ensureBrushProgressTrack(km);
    m_timeline->setClipKeyframes(km);

    syncBrushAnimationPreviewForClip(trackIdx, clipIdx);

    statusBar()->showMessage(QString("ブラシアニメを追加: 「%1」 (%2)")
        .arg(params.text, params.mode == BrushAnimationMode::PerCharacter
            ? QStringLiteral("Per Character") : QStringLiteral("Per Stroke")));
}

void MainWindow::editTransformKeyframes()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Transform", "Select a clip first.");
        return;
    }

    QStringList properties = {"Position X", "Position Y", "Scale X", "Scale Y",
                              "Rotation", "Opacity", "Skew X", "Skew Y"};
    bool ok;
    QString prop = QInputDialog::getItem(this, "Transform Keyframe",
        "Property to animate:", properties, 0, false, &ok);
    if (!ok) return;

    double time = m_timeline->playheadPosition();
    double value = QInputDialog::getDouble(this, "Transform Keyframe",
        QString("Value for %1 at %2s:").arg(prop).arg(time, 0, 'f', 1),
        0.0, -10000, 10000, 2, &ok);
    if (!ok) return;

    statusBar()->showMessage(QString("Set keyframe: %1 = %2 at %3s")
        .arg(prop).arg(value).arg(time, 0, 'f', 1));
}

void MainWindow::addMask()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Mask", "Select a clip first.");
        return;
    }

    QStringList shapes = {"Rectangle", "Ellipse", "Polygon"};
    bool ok;
    QString selected = QInputDialog::getItem(this, "Add Mask",
        "Mask shape:", shapes, 0, false, &ok);
    if (!ok) return;

    double feather = QInputDialog::getDouble(this, "Mask Feather",
        "Feather amount (pixels):", 10.0, 0.0, 100.0, 1, &ok);
    if (!ok) return;

    statusBar()->showMessage(QString("Added %1 mask (feather: %2px)").arg(selected).arg(feather));
}

void MainWindow::applyWarpEffect()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Warp", "Select a clip first.");
        return;
    }

    QStringList warps = {"Mesh Warp", "Puppet Pin", "Bulge", "Pinch", "Twirl",
                         "Wave", "Ripple", "Spherize", "Fisheye"};
    bool ok;
    QString selected = QInputDialog::getItem(this, "Warp / Distortion",
        "Effect type:", warps, 0, false, &ok);
    if (!ok) return;

    double amount = QInputDialog::getDouble(this, "Warp Amount",
        "Amount (0.0-1.0):", 0.5, 0.0, 2.0, 2, &ok);
    if (!ok) return;

    statusBar()->showMessage(QString("Applied %1 (amount: %2)").arg(selected).arg(amount, 0, 'f', 2));
}

void MainWindow::editExpressions()
{
    bool ok;
    QString propName = QInputDialog::getText(this, "Expression",
        "Property name (e.g., rotation, opacity):", QLineEdit::Normal, "rotation", &ok);
    if (!ok || propName.isEmpty()) return;

    QString code = QInputDialog::getText(this, "Expression",
        "Expression code:", QLineEdit::Normal, "wiggle(2, 10)", &ok);
    if (!ok || code.isEmpty()) return;

    QString validationError = Expression::validate(code);
    if (!validationError.isEmpty()) {
        QMessageBox::warning(this, "Expression Error", "Invalid expression: " + validationError);
        return;
    }

    statusBar()->showMessage(QString("Expression set: %1 = %2").arg(propName, code));
}

void MainWindow::precomposeSelected()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Pre-Compose", "Select clips first.");
        return;
    }

    bool ok;
    QString name = QInputDialog::getText(this, "Pre-Compose",
        "Composition name:", QLineEdit::Normal, "Comp 1", &ok);
    if (!ok || name.isEmpty()) return;

    int compId = m_precomposeManager.createComposition(name,
        m_projectConfig.width, m_projectConfig.height,
        m_projectConfig.fps, 10.0);
    statusBar()->showMessage(QString("Created composition: %1 (ID: %2)").arg(name).arg(compId));
}

void MainWindow::showResourceGuide()
{
    ResourceGuideDialog dialog(this);
    dialog.exec();
}

void MainWindow::about()
{
    QMessageBox::about(this, "About V Simple Editor",
        QString("V Simple Editor v%1\n\n"
                "A full-featured video editor with 90+ features.\n"
                "Built with C++17 / Qt6 / FFmpeg / OpenGL 3.3.\n\n"
                "Features: multi-track timeline, AE-style compositing,\n"
                "AI editing tools, GPU preview, 13 export presets,\n"
                "Python scripting, VST/AU plugins, and more.\n\n"
                "Press Ctrl+Alt+K to customize keyboard shortcuts.")
            .arg(APP_VERSION));
}

// --- Phase 14: New slot implementations ---

void MainWindow::setupRecentFiles()
{
    m_recentFilesManager = new RecentFilesManager(this);
}

void MainWindow::openRecentFile(const QString &filePath)
{
    QFileInfo fi(filePath);
    if (!fi.exists()) {
        QMessageBox::warning(this, "File Not Found",
            QString("The file '%1' no longer exists.").arg(filePath));
        if (m_recentFilesManager)
            m_recentFilesManager->removeFile(filePath);
        return;
    }

    // Check if it's a project file or media file
    if (filePath.endsWith(".veditor", Qt::CaseInsensitive)) {
        ProjectData data;
        if (ProjectFile::load(filePath, data)) {
            m_projectConfig = data.config;
            if (m_player)
                m_player->setCanvasSize(data.config.width, data.config.height);
            if (m_timeline)
                m_timeline->restoreFromProject(data.videoTracks, data.audioTracks,
                    data.playheadPos, data.markIn, data.markOut, data.zoomLevel);
            rebuildAudioMeters();
            applyAudioState(data);
            m_projectFilePath = filePath;
            if (m_recentFilesManager)
                m_recentFilesManager->addFile(filePath);
            updateTitle();
            hideWelcomeScreen();
            updateStatusInfo();
            updateEditActions();
            statusBar()->showMessage("Opened project: " + fi.fileName());
        }
    } else {
        loadMediaFile(filePath, false, "Opened");
    }
}

void MainWindow::editShortcuts()
{
    ShortcutEditorDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        ShortcutManager::instance().saveShortcuts();
        statusBar()->showMessage("Keyboard shortcuts updated");
    }
}

void MainWindow::applyShaderEffect()
{
    auto &lib = ShaderEffectLibrary::instance();
    auto effects = lib.allEffects();

    QStringList names;
    for (auto &e : effects)
        names << QString("%1 — %2").arg(e.category, e.name);

    bool ok;
    QString selected = QInputDialog::getItem(this, "GPU Shader Effect",
        "Select a shader effect to apply:", names, 0, false, &ok);
    if (!ok || selected.isEmpty()) return;

    int idx = names.indexOf(selected);
    if (idx >= 0 && idx < effects.size()) {
        statusBar()->showMessage(QString("Applied GPU shader: %1").arg(effects[idx].name));
    }
}

void MainWindow::manageShaderEffects()
{
    auto &lib = ShaderEffectLibrary::instance();
    auto effects = lib.allEffects();

    QString info = QString("GPU Shader Effect Library\n\n%1 effects available:\n\n").arg(effects.size());
    for (auto &cat : lib.categories()) {
        info += "  " + cat + ":\n";
        for (auto &e : lib.effectsByCategory(cat)) {
            info += "    - " + e.name + ": " + e.description + "\n";
        }
        info += "\n";
    }
    QMessageBox::information(this, "GPU Shader Effects", info);
}

void MainWindow::openVSTPlugins()
{
    VSTPluginDialog dialog(this);
    dialog.exec();
}

void MainWindow::openScriptConsole()
{
    ScriptConsole console(m_scriptEngine, &m_scriptManager, this);
    console.exec();
}

void MainWindow::openNetworkRender()
{
    NetworkRenderDialog dialog(&m_networkRenderServer, this);
    dialog.exec();
}

void MainWindow::exportToRemotion()
{
    ProjectData data;
    data.config = m_projectConfig;
    data.videoTracks = m_timeline->allVideoTracks();
    data.audioTracks = m_timeline->allAudioTracks();
    data.playheadPos = m_timeline->playheadPosition();
    data.markIn = m_timeline->markedIn();
    data.markOut = m_timeline->markedOut();

    RemotionExportDialog dialog(m_projectConfig, data, this);
    dialog.exec();
}

// --- UI/UX improvements ---

void MainWindow::setupStatusBarWidgets()
{
    auto makeLabel = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setStyleSheet("QLabel { color: #888; padding: 0 8px; font-size: 11px; }");
        statusBar()->addPermanentWidget(label);
        return label;
    };

    m_statusResolution = makeLabel("1920x1080");
    m_statusFps = makeLabel("30 fps");
    m_statusDuration = makeLabel("00:00:00");
    m_statusTheme = makeLabel("Dark");
}

void MainWindow::updateStatusInfo()
{
    if (m_statusResolution)
        m_statusResolution->setText(QString("%1x%2").arg(m_projectConfig.width).arg(m_projectConfig.height));
    if (m_statusFps)
        m_statusFps->setText(QString("%1 fps").arg(m_projectConfig.fps));

    if (m_statusDuration) {
        const double totalDur = m_timeline ? m_timeline->totalDuration() : 0.0;
        int h = (int)(totalDur / 3600);
        int m = (int)(fmod(totalDur, 3600) / 60);
        int s = (int)(fmod(totalDur, 60));
        m_statusDuration->setText(QString("%1:%2:%3")
            .arg(h, 2, 10, QChar('0'))
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0')));
    }

    if (m_statusTheme)
        m_statusTheme->setText(ThemeManager::themeName(ThemeManager::instance().currentTheme()));
}

void MainWindow::testLoadFile(const QString &filePath)
{
    qInfo() << "MainWindow::testLoadFile" << filePath;
    loadMediaFile(filePath, true, "Loaded");
}

void MainWindow::testStartPlayback()
{
    qInfo() << "MainWindow::testStartPlayback";
    if (m_player)
        QMetaObject::invokeMethod(m_player, "play", Qt::QueuedConnection);
}

void MainWindow::loadMediaFile(const QString &filePath, bool addToTimeline, const QString &statusPrefix)
{
    qInfo() << "MainWindow::loadMediaFile" << filePath << "addToTimeline=" << addToTimeline;
    if (filePath.isEmpty())
        return;

    const QFileInfo fi(filePath);
    if (addToTimeline && m_timeline)
        m_timeline->addClip(filePath);
    if (m_recentFilesManager)
        m_recentFilesManager->addFile(filePath);

    hideWelcomeScreen();
    updateStatusInfo();
    updateEditActions();
    statusBar()->showMessage("Loading: " + fi.fileName());

    // When the file is added to the timeline, the player is driven by the
    // Timeline::sequenceChanged → VideoPlayer::setSequence wiring (synchronous
    // from addClip above), so we must NOT call loadFile() again here — that
    // would clobber the sequence-driven load with a single-file load.
    const QString playbackPath = ProxyManager::instance().getProxyPath(filePath);
    const bool needsDirectLoad = !addToTimeline;
    QPointer<MainWindow> guard(this);
    QTimer::singleShot(0, this, [guard, filePath, playbackPath, statusPrefix, needsDirectLoad]() {
        if (!guard)
            return;
        qInfo() << "MainWindow::loadMediaFile deferred load" << playbackPath
                << "directLoad=" << needsDirectLoad;
        if (needsDirectLoad && guard->m_player)
            guard->m_player->loadFile(playbackPath);
        guard->updateStatusInfo();
        guard->updateEditActions();
        guard->statusBar()->showMessage(statusPrefix + ": " + QFileInfo(filePath).fileName());
    });
}

void MainWindow::showWelcomeScreen()
{
    if (m_welcomeWidget) {
        if (m_recentFilesManager)
            m_welcomeWidget->setRecentFiles(m_recentFilesManager->recentFiles());
        m_welcomeWidget->setVisible(true);
    }
    if (m_mainSplitter)
        m_mainSplitter->setVisible(false);
    m_hasContent = false;
}

void MainWindow::hideWelcomeScreen()
{
    if (m_hasContent)
        return;
    if (m_welcomeWidget)
        m_welcomeWidget->setVisible(false);
    if (m_mainSplitter)
        m_mainSplitter->setVisible(true);
    m_hasContent = true;
}

void MainWindow::saveWindowState()
{
    QSettings settings("VSimpleEditor", "WindowState");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
    if (m_mainSplitter)
        settings.setValue("splitterState", m_mainSplitter->saveState());
    settings.setValue("theme", (int)ThemeManager::instance().currentTheme());
    QSettings uiSettings("VSimpleEditor", "UI");
    uiSettings.setValue("historyDockVisible", m_historyDock ? m_historyDock->isVisible() : true);
}

void MainWindow::restoreWindowState()
{
    QSettings settings("VSimpleEditor", "WindowState");
    if (settings.contains("geometry")) {
        restoreGeometry(settings.value("geometry").toByteArray());
    }
    if (settings.contains("windowState")) {
        restoreState(settings.value("windowState").toByteArray());
    }
    // カラーグレーディングパネルは常に非表示で起動
    if (m_colorGradingPanel)
        m_colorGradingPanel->close();
    if (m_mainSplitter && settings.contains("splitterState")) {
        m_mainSplitter->restoreState(settings.value("splitterState").toByteArray());
    }
    if (settings.contains("theme")) {
        auto themeType = (ThemeType)settings.value("theme").toInt();
        ThemeManager::instance().applyTheme(themeType, this);
    }
    if (m_historyDock) {
        QSettings uiSettings("VSimpleEditor", "UI");
        m_historyDock->setVisible(uiSettings.value("historyDockVisible", true).toBool());
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        for (auto &url : event->mimeData()->urls()) {
            QString path = url.toLocalFile().toLower();
            if (path.endsWith(".mp4") || path.endsWith(".mkv") || path.endsWith(".mov") ||
                path.endsWith(".webm") || path.endsWith(".flv") || path.endsWith(".veditor") ||
                path.endsWith(".png") || path.endsWith(".jpg") || path.endsWith(".jpeg") ||
                path.endsWith(".wav") || path.endsWith(".mp3") || path.endsWith(".aac")) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    for (auto &url : event->mimeData()->urls()) {
        QString filePath = url.toLocalFile();
        if (filePath.isEmpty()) continue;

        if (filePath.endsWith(".veditor", Qt::CaseInsensitive)) {
            // Open as project
            ProjectData data;
            if (ProjectFile::load(filePath, data)) {
                m_projectFilePath = filePath;
                if (m_recentFilesManager)
                    m_recentFilesManager->addFile(filePath);
                m_projectConfig = data.config;
                if (m_player)
                    m_player->setCanvasSize(data.config.width, data.config.height);
                if (m_timeline)
                    m_timeline->restoreFromProject(data.videoTracks, data.audioTracks,
                        data.playheadPos, data.markIn, data.markOut, data.zoomLevel);
                rebuildAudioMeters();
                applyAudioState(data);
                updateTitle();
                hideWelcomeScreen();
                updateStatusInfo();
                statusBar()->showMessage("Opened project: " + QFileInfo(filePath).fileName());
            }
        } else {
            // Open as media file
            loadMediaFile(filePath, true, "Loaded");
        }
    }
    updateEditActions();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);

    if (!m_autoSaveStarted && m_autoSave) {
        m_autoSaveStarted = true;
        QTimer::singleShot(500, this, [this]() {
            if (!m_autoSave)
                return;
            // Default OFF. Interval persisted via QSettings, default 30 min.
            QSettings prefSettings("VSimpleEditor", "Preferences");
            const bool enabled = prefSettings.value("autoSaveEnabled", false).toBool();
            if (!enabled)
                return;
            AutoSaveConfig cfg;
            cfg.enabled = true;
            cfg.interval = prefSettings.value("autoSaveIntervalSec", 1800).toInt();
            m_autoSave->start(cfg);
        });
    }
}

void MainWindow::rebuildAudioMeters()
{
    if (!m_audioMetersDock || !m_player)
        return;

    // Disconnect all existing meter connections before creating new ones
    for (const auto &conn : m_meterConnections)
        QObject::disconnect(conn);
    m_meterConnections.clear();
    m_audioMeterWidgets.clear();

    const int n = m_timeline ? m_timeline->audioTrackCount() : 0;

    QWidget *container = new QWidget();
    QHBoxLayout *layout = new QHBoxLayout(container);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    for (int i = 0; i < n; ++i) {
        QVBoxLayout *trackLayout = new QVBoxLayout();
        trackLayout->setSpacing(2);

        QLabel *label = new QLabel(QString("T%1").arg(i + 1));
        label->setAlignment(Qt::AlignCenter);

        AudioMeterWidget *meter = new AudioMeterWidget();
        meter->setFixedWidth(20);
        meter->setTrackIndex(i);

        trackLayout->addWidget(label);
        trackLayout->addWidget(meter);
        layout->addLayout(trackLayout);

        m_audioMeterWidgets.append(meter);

        if (auto *mixer = m_player->audioMixer()) {
            auto conn = connect(mixer, &AudioMixer::levelChanged, meter,
                    [meter, i](int trackIdx, float pkL, float pkR, float rmsL, float rmsR) {
                        if (trackIdx == i)
                            meter->setLevels(pkL, pkR, rmsL, rmsR);
                    });
            m_meterConnections.append(conn);
        }

        connect(meter, &AudioMeterWidget::requestEqPresetMenu,
                this, &MainWindow::onMeterRequestEqPresetMenu);
        connect(meter, &AudioMeterWidget::requestCompressorDialog,
                this, &MainWindow::onMeterRequestCompressorDialog);
        connect(meter, &AudioMeterWidget::requestAutoDuckDialog,
                this, &MainWindow::onMeterRequestAutoDuckDialog);
        connect(meter, &AudioMeterWidget::requestNormalize,
                this, &MainWindow::onMeterRequestNormalize);
    }

    if (n > 0) {
        QFrame *sep = new QFrame();
        sep->setFrameShape(QFrame::VLine);
        layout->addWidget(sep);
    }

    // Master meter
    QVBoxLayout *masterLayout = new QVBoxLayout();
    masterLayout->setSpacing(2);

    QLabel *masterLabel = new QLabel("Master");
    masterLabel->setAlignment(Qt::AlignCenter);

    AudioMeterWidget *masterMeter = new AudioMeterWidget();
    masterMeter->setFixedWidth(20);

    masterLayout->addWidget(masterLabel);
    masterLayout->addWidget(masterMeter);
    layout->addLayout(masterLayout);

    m_masterMeter = masterMeter;

    if (auto *mixer = m_player->audioMixer()) {
        auto conn = connect(mixer, &AudioMixer::masterLevelChanged, masterMeter,
                &AudioMeterWidget::setLevels);
        m_meterConnections.append(conn);
    }

    connect(masterMeter, &AudioMeterWidget::requestCompressorDialog,
            this, &MainWindow::onMeterRequestCompressorDialog);
    connect(masterMeter, &AudioMeterWidget::requestNormalizeAll,
            this, &MainWindow::onMeterRequestNormalizeAll);
    connect(masterMeter, &AudioMeterWidget::requestResetAllMeters,
            this, &MainWindow::onMeterRequestResetAllMeters);

    layout->addStretch();

    QWidget *oldWidget = m_audioMetersDock->widget();
    m_audioMetersDock->setWidget(container);
    if (oldWidget)
        oldWidget->deleteLater();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveWindowState();
    if (m_autoSave) m_autoSave->markCleanShutdown();
    QMainWindow::closeEvent(event);
}

// --- Effects menu quick-access for Sharpen / Mosaic / ChromaKey ---

void MainWindow::applySharpenEffect()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Sharpen", "Select a clip first.");
        return;
    }
    auto effects = m_timeline->clipEffects();
    effects.append(VideoEffect::createSharpen());
    m_timeline->setClipEffects(effects);
    if (m_player)
        m_player->setPreviewEffects(effects, true);
    statusBar()->showMessage("シャープンを適用しました", 3000);
}

void MainWindow::applyMosaicEffect()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Mosaic", "Select a clip first.");
        return;
    }
    auto effects = m_timeline->clipEffects();
    effects.append(VideoEffect::createMosaic());
    m_timeline->setClipEffects(effects);
    if (m_player)
        m_player->setPreviewEffects(effects, true);
    statusBar()->showMessage("モザイクを適用しました", 3000);
}

void MainWindow::applyChromaKeyEffect()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Chroma Key", "Select a clip first.");
        return;
    }
    auto effects = m_timeline->clipEffects();
    effects.append(VideoEffect::createChromaKey());
    m_timeline->setClipEffects(effects);
    if (m_player)
        m_player->setPreviewEffects(effects, true);
    statusBar()->showMessage("クロマキーを適用しました", 3000);
}

// --- Master Compressor inline dialog ---

void MainWindow::openMasterCompressor()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, "Compressor", "Audio mixer unavailable.");
        return;
    }

    const AudioMixer::CompressorParams current = mixer->compressorParams();

    QDialog dlg(this);
    dlg.setWindowTitle("Master Compressor");
    auto *form = new QFormLayout(&dlg);

    // Threshold
    auto *thresholdSlider = new QSlider(Qt::Horizontal, &dlg);
    thresholdSlider->setRange(-30, 0);
    thresholdSlider->setValue(static_cast<int>(current.thresholdDb));
    auto *thresholdSpin = new QSpinBox(&dlg);
    thresholdSpin->setRange(-30, 0);
    thresholdSpin->setSuffix(" dB");
    thresholdSpin->setValue(static_cast<int>(current.thresholdDb));
    connect(thresholdSlider, &QSlider::valueChanged, thresholdSpin, &QSpinBox::setValue);
    connect(thresholdSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            thresholdSlider, &QSlider::setValue);
    auto *thresholdRow = new QHBoxLayout();
    thresholdRow->addWidget(thresholdSlider);
    thresholdRow->addWidget(thresholdSpin);
    form->addRow("Threshold", thresholdRow);

    // Ratio
    auto *ratioSlider = new QSlider(Qt::Horizontal, &dlg);
    ratioSlider->setRange(10, 200); // 1.0–20.0 × 10
    ratioSlider->setValue(static_cast<int>(current.ratio * 10.0));
    auto *ratioSpin = new QDoubleSpinBox(&dlg);
    ratioSpin->setRange(1.0, 20.0);
    ratioSpin->setSingleStep(0.5);
    ratioSpin->setDecimals(1);
    ratioSpin->setSuffix(" :1");
    ratioSpin->setValue(current.ratio);
    connect(ratioSlider, &QSlider::valueChanged, this,
            [ratioSpin](int v) { ratioSpin->setValue(v / 10.0); });
    connect(ratioSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [ratioSlider](double v) { ratioSlider->setValue(static_cast<int>(v * 10.0)); });
    auto *ratioRow = new QHBoxLayout();
    ratioRow->addWidget(ratioSlider);
    ratioRow->addWidget(ratioSpin);
    form->addRow("Ratio", ratioRow);

    // Attack
    auto *attackSlider = new QSlider(Qt::Horizontal, &dlg);
    attackSlider->setRange(1, 100);
    attackSlider->setValue(static_cast<int>(current.attackMs));
    auto *attackSpin = new QDoubleSpinBox(&dlg);
    attackSpin->setRange(1.0, 100.0);
    attackSpin->setSingleStep(1.0);
    attackSpin->setDecimals(1);
    attackSpin->setSuffix(" ms");
    attackSpin->setValue(current.attackMs);
    connect(attackSlider, &QSlider::valueChanged, this,
            [attackSpin](int v) { attackSpin->setValue(static_cast<double>(v)); });
    connect(attackSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [attackSlider](double v) { attackSlider->setValue(static_cast<int>(v)); });
    auto *attackRow = new QHBoxLayout();
    attackRow->addWidget(attackSlider);
    attackRow->addWidget(attackSpin);
    form->addRow("Attack", attackRow);

    // Release
    auto *releaseSlider = new QSlider(Qt::Horizontal, &dlg);
    releaseSlider->setRange(10, 1000);
    releaseSlider->setValue(static_cast<int>(current.releaseMs));
    auto *releaseSpin = new QDoubleSpinBox(&dlg);
    releaseSpin->setRange(10.0, 1000.0);
    releaseSpin->setSingleStep(10.0);
    releaseSpin->setDecimals(0);
    releaseSpin->setSuffix(" ms");
    releaseSpin->setValue(current.releaseMs);
    connect(releaseSlider, &QSlider::valueChanged, this,
            [releaseSpin](int v) { releaseSpin->setValue(static_cast<double>(v)); });
    connect(releaseSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [releaseSlider](double v) { releaseSlider->setValue(static_cast<int>(v)); });
    auto *releaseRow = new QHBoxLayout();
    releaseRow->addWidget(releaseSlider);
    releaseRow->addWidget(releaseSpin);
    form->addRow("Release", releaseRow);

    // Makeup gain
    auto *makeupSlider = new QSlider(Qt::Horizontal, &dlg);
    makeupSlider->setRange(0, 18);
    makeupSlider->setValue(static_cast<int>(current.makeupDb));
    auto *makeupSpin = new QDoubleSpinBox(&dlg);
    makeupSpin->setRange(0.0, 18.0);
    makeupSpin->setSingleStep(0.5);
    makeupSpin->setDecimals(1);
    makeupSpin->setSuffix(" dB");
    makeupSpin->setValue(current.makeupDb);
    connect(makeupSlider, &QSlider::valueChanged, this,
            [makeupSpin](int v) { makeupSpin->setValue(static_cast<double>(v)); });
    connect(makeupSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [makeupSlider](double v) { makeupSlider->setValue(static_cast<int>(v)); });
    auto *makeupRow = new QHBoxLayout();
    makeupRow->addWidget(makeupSlider);
    makeupRow->addWidget(makeupSpin);
    form->addRow("Makeup Gain", makeupRow);

    // Enable checkbox
    auto *enableCheck = new QCheckBox("Enable Compressor", &dlg);
    enableCheck->setChecked(current.enabled);
    form->addRow(enableCheck);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() == QDialog::Accepted) {
        AudioMixer::CompressorParams params;
        params.thresholdDb = static_cast<double>(thresholdSpin->value());
        params.ratio = ratioSpin->value();
        params.attackMs = attackSpin->value();
        params.releaseMs = releaseSpin->value();
        params.makeupDb = makeupSpin->value();
        params.enabled = enableCheck->isChecked();
        mixer->setCompressorParams(params);
        statusBar()->showMessage(
            QString("Compressor %1 (threshold %2 dB, ratio %3:1)")
                .arg(params.enabled ? "ON" : "OFF")
                .arg(params.thresholdDb, 0, 'f', 0)
                .arg(params.ratio, 0, 'f', 1),
            4000);
    }
}

void MainWindow::openAutoDuckSettings()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, "Auto-Duck", "Audio mixer unavailable.");
        return;
    }

    const AudioMixer::AutoDuckParams current = mixer->autoDuckParams();

    QDialog dlg(this);
    dlg.setWindowTitle("Auto-Duck Settings");
    auto *form = new QFormLayout(&dlg);

    // Threshold
    auto *thresholdSpin = new QDoubleSpinBox(&dlg);
    thresholdSpin->setRange(-60.0, 0.0);
    thresholdSpin->setSingleStep(1.0);
    thresholdSpin->setDecimals(1);
    thresholdSpin->setSuffix(" dB");
    thresholdSpin->setValue(current.thresholdDb);
    form->addRow("Threshold", thresholdSpin);

    // Ratio
    auto *ratioSpin = new QDoubleSpinBox(&dlg);
    ratioSpin->setRange(1.0, 20.0);
    ratioSpin->setSingleStep(0.5);
    ratioSpin->setDecimals(1);
    ratioSpin->setSuffix(" :1");
    ratioSpin->setValue(current.ratio);
    form->addRow("Ratio", ratioSpin);

    // Attack
    auto *attackSpin = new QDoubleSpinBox(&dlg);
    attackSpin->setRange(0.1, 5000.0);
    attackSpin->setSingleStep(1.0);
    attackSpin->setDecimals(1);
    attackSpin->setSuffix(" ms");
    attackSpin->setValue(current.attackMs);
    form->addRow("Attack", attackSpin);

    // Release
    auto *releaseSpin = new QDoubleSpinBox(&dlg);
    releaseSpin->setRange(1.0, 10000.0);
    releaseSpin->setSingleStep(10.0);
    releaseSpin->setDecimals(0);
    releaseSpin->setSuffix(" ms");
    releaseSpin->setValue(current.releaseMs);
    form->addRow("Release", releaseSpin);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() == QDialog::Accepted) {
        AudioMixer::AutoDuckParams params;
        params.thresholdDb = thresholdSpin->value();
        params.ratio = ratioSpin->value();
        params.attackMs = attackSpin->value();
        params.releaseMs = releaseSpin->value();
        mixer->setAutoDuckParams(params);
        statusBar()->showMessage(
            QString("Auto-Duck: threshold %1 dB, ratio %2:1, attack %3 ms, release %4 ms")
                .arg(params.thresholdDb, 0, 'f', 1)
                .arg(params.ratio, 0, 'f', 1)
                .arg(params.attackMs, 0, 'f', 1)
                .arg(params.releaseMs, 0, 'f', 0),
            4000);
    }
}

// --- Audio meter context menu handlers ---

void MainWindow::onMeterRequestEqPresetMenu(int trackIdx, QPoint globalPos)
{
    if (!m_player) return;
    auto *mixer = m_player->audioMixer();
    if (!mixer) return;

    QMenu menu;
    const auto presets = AudioEQProcessor::presets();
    const int ti = trackIdx;
    for (const auto &preset : presets) {
        auto *act = menu.addAction(preset.name);
        const AudioEQConfig cfg = preset.config;
        const QString presetName = preset.name;
        connect(act, &QAction::triggered, this,
                [this, mixer, ti, cfg, presetName]() {
            if (mixer) {
                mixer->setTrackEqEnabled(ti, true);
                mixer->setTrackEqConfig(ti, cfg);
            }
            statusBar()->showMessage(
                QString("A%1 EQ → %2").arg(ti + 1).arg(presetName), 3000);
        });
    }
    menu.addSeparator();
    auto *bypassAct = menu.addAction("Bypass / Reset");
    const int ti2 = trackIdx;
    connect(bypassAct, &QAction::triggered, this,
            [this, mixer, ti2]() {
        if (mixer) {
            mixer->setTrackEqEnabled(ti2, false);
        }
        statusBar()->showMessage(
            QString("A%1 EQ bypassed").arg(ti2 + 1), 3000);
    });

    menu.exec(globalPos);
}

void MainWindow::onMeterRequestCompressorDialog()
{
    openMasterCompressor();
}

void MainWindow::onMeterRequestAutoDuckDialog()
{
    openAutoDuckSettings();
}

void MainWindow::onMeterRequestNormalize(int trackIdx, double gainDb)
{
    if (!m_player) return;
    auto *mixer = m_player->audioMixer();
    if (!mixer) return;

    if (std::abs(gainDb) < 0.001) {
        statusBar()->showMessage(
            QStringLiteral("メーターに信号がありません. 一度再生してから実行してください."), 3000);
        return;
    }

    m_timeline->undoManager()->saveState(m_timeline->currentState(), "Normalize audio");

    const double oldGain = mixer->trackGain(trackIdx);
    const double factor = std::pow(10.0, gainDb / 20.0);
    const double newGain = std::clamp(oldGain * factor, 0.0, 4.0);
    mixer->setTrackGain(trackIdx, newGain);

    statusBar()->showMessage(
        QString("T%1 ノーマライズ: +%2dB (gain %3→%4)")
            .arg(trackIdx + 1)
            .arg(gainDb, 0, 'f', 1)
            .arg(oldGain, 0, 'f', 2)
            .arg(newGain, 0, 'f', 2),
        4000);
}

void MainWindow::onMeterRequestNormalizeAll()
{
    if (!m_player) return;
    auto *mixer = m_player->audioMixer();
    if (!mixer) return;

    bool anyApplied = false;
    for (int i = 0; i < m_audioMeterWidgets.size(); ++i) {
        const double peak = m_audioMeterWidgets[i]->currentPeakHoldDb();
        if (peak <= -60.0)
            continue;
        const double gainDb = std::clamp(-1.0 - peak, -24.0, 12.0);
        if (std::abs(gainDb) < 0.001)
            continue;
        if (!anyApplied)
            m_timeline->undoManager()->saveState(m_timeline->currentState(), "Normalize all audio");
        const double oldGain = mixer->trackGain(i);
        const double factor = std::pow(10.0, gainDb / 20.0);
        const double newGain = std::clamp(oldGain * factor, 0.0, 4.0);
        mixer->setTrackGain(i, newGain);
        anyApplied = true;
    }

    if (anyApplied) {
        statusBar()->showMessage(
            QStringLiteral("全トラックをノーマライズしました"), 4000);
    } else {
        statusBar()->showMessage(
            QStringLiteral("メーターに信号がありません. 一度再生してから実行してください."), 3000);
    }
}

void MainWindow::onMeterRequestResetAllMeters()
{
    for (auto *meter : m_audioMeterWidgets)
        meter->resetPeakHold();
    if (m_masterMeter)
        m_masterMeter->resetPeakHold();
}

// =============================================================
// Consolidation slots — wire panels and dialogs implemented in
// earlier sprint stories into the menu bar.
// =============================================================

namespace {
// Build the (itemNames, trackIds) lists EqualizerPanel::setTracks expects.
// Convention: id 0 = Master, ids 1..N = A1..An audio tracks (in order).
static void buildAudioTrackList(int audioTrackCount,
                                QStringList &itemNames,
                                QList<int> &trackIds,
                                bool includeMaster = true)
{
    itemNames.clear();
    trackIds.clear();
    if (includeMaster) {
        itemNames << QStringLiteral("Master");
        trackIds << 0;
    }
    for (int i = 0; i < audioTrackCount; ++i) {
        itemNames << QStringLiteral("A%1").arg(i + 1);
        trackIds << (i + 1);
    }
}
} // namespace

void MainWindow::openEqualizerPanel()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, tr("EQ パネル"),
            tr("オーディオミキサーが利用できません。"));
        return;
    }

    if (!m_equalizerDock) {
        m_equalizerDock = new QDockWidget(tr("EQ"), this);
        m_equalizerDock->setObjectName("EqualizerDock");
        auto *panel = new EqualizerPanel(m_equalizerDock);
        m_equalizerDock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, m_equalizerDock);

        connect(panel, &EqualizerPanel::eqChanged,
                this, [this](int trackId, AudioMixer::EqSettings eq) {
            if (auto *mx = m_player ? m_player->audioMixer() : nullptr)
                mx->setEqForTrack(trackId, eq);
        });
    }

    // Re-seed track list and current per-track settings on every show
    // so newly added audio tracks appear without restart.
    if (auto *panel = qobject_cast<EqualizerPanel *>(m_equalizerDock->widget())) {
        QStringList names;
        QList<int> ids;
        const int trackCount = m_timeline ? m_timeline->audioTrackCount() : 0;
        buildAudioTrackList(trackCount, names, ids);
        panel->setTracks(names, ids);
        for (int id : ids)
            panel->setEqSettings(id, mixer->eqForTrack(id));
    }
    m_equalizerDock->setVisible(true);
    m_equalizerDock->raise();
}

void MainWindow::openCompressorPanel()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, tr("コンプレッサー"),
            tr("オーディオミキサーが利用できません。"));
        return;
    }

    if (!m_compressorDock) {
        m_compressorDock = new QDockWidget(tr("Compressor / Limiter"), this);
        m_compressorDock->setObjectName("CompressorDock");
        auto *panel = new CompressorPanel(m_compressorDock);
        panel->setMixer(mixer);
        m_compressorDock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, m_compressorDock);

        connect(panel, &CompressorPanel::compressorChanged,
                this, [this](int trackId, AudioMixer::CompressorSettings c) {
            if (auto *mx = m_player ? m_player->audioMixer() : nullptr)
                mx->setCompressorForTrack(trackId, c);
        });
    }

    if (auto *panel = qobject_cast<CompressorPanel *>(m_compressorDock->widget())) {
        QStringList names;
        QList<int> ids;
        const int trackCount = m_timeline ? m_timeline->audioTrackCount() : 0;
        buildAudioTrackList(trackCount, names, ids, /*includeMaster=*/false);
        // CompressorPanel::setTrackList accepts the active track ids and
        // inserts the master row (id 0) itself.
        panel->setTrackList(ids);
        for (int id : ids)
            panel->loadSettings(id, mixer->compressorForTrack(id));
        panel->loadSettings(0, mixer->compressorForTrack(0));
    }
    m_compressorDock->setVisible(true);
    m_compressorDock->raise();
}

void MainWindow::openReverbPanel()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, tr("リバーブ"),
            tr("オーディオミキサーが利用できません。"));
        return;
    }

    if (!m_reverbDock) {
        m_reverbDock = new QDockWidget(tr("Reverb"), this);
        m_reverbDock->setObjectName("ReverbDock");
        auto *panel = new ReverbPanel(m_reverbDock);
        panel->setMixer(mixer);
        m_reverbDock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, m_reverbDock);

        connect(panel, &ReverbPanel::reverbChanged,
                this, [this](int trackId, AudioMixer::ReverbSettings r) {
            if (auto *mx = m_player ? m_player->audioMixer() : nullptr)
                mx->setReverbForTrack(trackId, r);
        });
    }

    if (auto *panel = qobject_cast<ReverbPanel *>(m_reverbDock->widget())) {
        QStringList names;
        QList<int> ids;
        const int trackCount = m_timeline ? m_timeline->audioTrackCount() : 0;
        buildAudioTrackList(trackCount, names, ids, /*includeMaster=*/false);
        panel->setTrackList(ids);
        for (int id : ids)
            panel->loadSettings(id, mixer->reverbForTrack(id));
        panel->loadSettings(0, mixer->reverbForTrack(0));
    }
    m_reverbDock->setVisible(true);
    m_reverbDock->raise();
}

void MainWindow::openNoiseReductionPanel()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, tr("ノイズリダクション"),
            tr("オーディオミキサーが利用できません。"));
        return;
    }

    if (!m_noiseReductionDock) {
        m_noiseReductionDock = new QDockWidget(tr("Noise Reduction"), this);
        m_noiseReductionDock->setObjectName("NoiseReductionDock");
        auto *panel = new NoiseReductionPanel(m_noiseReductionDock);
        panel->setMixer(mixer);
        m_noiseReductionDock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, m_noiseReductionDock);

        connect(panel, &NoiseReductionPanel::noiseReductionChanged,
                this, [this](int trackId, AudioMixer::NoiseReductionSettings nr) {
            if (auto *mx = m_player ? m_player->audioMixer() : nullptr)
                mx->setNoiseReductionForTrack(trackId, nr);
        });
    }

    if (auto *panel = qobject_cast<NoiseReductionPanel *>(m_noiseReductionDock->widget())) {
        QStringList names;
        QList<int> ids;
        const int trackCount = m_timeline ? m_timeline->audioTrackCount() : 0;
        buildAudioTrackList(trackCount, names, ids, /*includeMaster=*/false);
        panel->setTrackList(ids);
        for (int id : ids)
            panel->loadSettings(id, mixer->noiseReductionForTrack(id));
        panel->loadSettings(0, mixer->noiseReductionForTrack(0));
    }
    m_noiseReductionDock->setVisible(true);
    m_noiseReductionDock->raise();
}

void MainWindow::openTitlePresetDialog()
{
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, tr("タイトルプリセット"),
            tr("先にクリップを追加してください。"));
        return;
    }

    TitlePresetDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const EnhancedTextOverlay &resolved = dlg.resolvedOverlay();
    if (m_timeline->videoClips().isEmpty())
        return;

    // Persist the resolved overlay into V1's first clip via the
    // existing helper (writes back via setClips internally so the
    // mutation actually sticks).
    if (!m_timeline->addTextOverlayToFirstVideoClip(resolved)) {
        statusBar()->showMessage(
            tr("タイトルプリセットの適用に失敗しました"), 3000);
        return;
    }
    statusBar()->showMessage(
        tr("タイトルプリセットを適用しました: %1").arg(resolved.text), 4000);
}

void MainWindow::openMultiCamDialog()
{
    MultiCamDialog dlg(this);
    if (m_timeline) {
        const qint64 totalUs =
            static_cast<qint64>(m_timeline->totalDuration() * 1000000.0);
        if (totalUs > 0)
            dlg.setTimelineDurationUs(totalUs);
    }
    connect(&dlg, &MultiCamDialog::applyToTimeline,
            this, &MainWindow::onMultiCamApplyToTimeline);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const MultiCamProject project = dlg.result();
    statusBar()->showMessage(
        tr("マルチカメラ EDL を作成しました (角度: %1, 切替: %2)")
            .arg(project.angles.size())
            .arg(project.switches.size()), 4000);
}

void MainWindow::onMultiCamApplyToTimeline(const MultiCamProject &project)
{
    if (!m_timeline) return;
    if (project.switches.isEmpty() || project.angles.isEmpty()) {
        statusBar()->showMessage(
            tr("マルチカメラ: 切替マーカーが無いため適用をスキップしました"), 4000);
        return;
    }

    if (!m_timeline->videoClips().isEmpty()) {
        const auto reply = QMessageBox::question(
            this, tr("マルチカメラ"),
            tr("V1 トラックを multi-cam EDL で置き換えますか？\n"
               "(現在 %1 個のクリップが消去されます)")
                .arg(m_timeline->videoClips().size()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;
    }

    // angle id → angle lookup. Switches reference angles by id (not by
    // index) so the EDL survives angle reorder/remove in the dialog.
    QHash<int, MultiCamAngle> angleById;
    for (const MultiCamAngle &a : project.angles)
        angleById.insert(a.id, a);

    // Defensive sort — the dialog already keeps switches[] ordered, but a
    // mis-built project loaded from JSON could violate that.
    QVector<MultiCamSwitch> sw = project.switches;
    std::sort(sw.begin(), sw.end(),
              [](const MultiCamSwitch &a, const MultiCamSwitch &b) {
                  return a.timelineUs < b.timelineUs;
              });

    // Tail duration for the final switch — give the last angle a 30 s tail
    // since switches[] only encodes "when to cut TO" but never an explicit
    // EOF. Downstream EOF handling (VideoPlayer + AudioMixer) clamps to
    // the source file length on its own.
    constexpr qint64 kTailFallbackUs = 30LL * 1000000LL;

    QVector<ClipInfo> v1Clips;
    QVector<ClipInfo> a1Clips;
    int skipped = 0;

    qint64 prevTlEndUs = 0;
    for (int i = 0; i < sw.size(); ++i) {
        const MultiCamSwitch &cur = sw[i];
        if (!angleById.contains(cur.activeAngleId)) {
            ++skipped;
            continue;
        }
        const MultiCamAngle &angle = angleById.value(cur.activeAngleId);
        if (angle.sourcePath.isEmpty()
            || !QFileInfo::exists(angle.sourcePath)) {
            ++skipped;
            continue;
        }

        const qint64 segStartTlUs = cur.timelineUs;
        const qint64 segEndTlUs   = (i + 1 < sw.size())
            ? sw[i + 1].timelineUs
            : segStartTlUs + kTailFallbackUs;
        if (segEndTlUs <= segStartTlUs)
            continue;

        const qint64 segDurUs = segEndTlUs - segStartTlUs;
        // syncOffsetUs shifts the source PTS for this angle so multi-cam
        // sync (clap board / audio align) lands at the same timeline tick.
        const qint64 srcStartUs =
            std::max<qint64>(0, segStartTlUs - angle.syncOffsetUs);
        const qint64 leadInUs   = segStartTlUs - prevTlEndUs;

        ClipInfo videoClip;
        videoClip.filePath = angle.sourcePath;
        videoClip.displayName = QFileInfo(angle.sourcePath).fileName();
        const double srcStartSec = static_cast<double>(srcStartUs) / 1000000.0;
        const double segDurSec   = static_cast<double>(segDurUs)   / 1000000.0;
        videoClip.duration = srcStartSec + segDurSec;
        videoClip.inPoint  = srcStartSec;
        videoClip.outPoint = srcStartSec + segDurSec;
        videoClip.leadInSec = static_cast<double>(leadInUs) / 1000000.0;

        v1Clips.append(videoClip);

        // Mirror the same source range to A1 — most camera files carry
        // embedded audio, and AudioMixer silently outputs zero samples
        // for missing audio streams.
        a1Clips.append(videoClip);

        prevTlEndUs = segEndTlUs;
    }

    if (v1Clips.isEmpty()) {
        statusBar()->showMessage(
            tr("マルチカメラ: 有効なセグメントが無く適用を中止しました"), 4000);
        return;
    }

    while (m_timeline->videoTrackCount() < 1)
        m_timeline->addVideoTrack();
    while (m_timeline->audioTrackCount() < 1)
        m_timeline->addAudioTrack();

    m_timeline->videoTracks().first()->setClips(v1Clips);
    m_timeline->audioTracks().first()->setClips(a1Clips);
    m_timeline->refreshPlaybackSequence();

    if (skipped > 0) {
        statusBar()->showMessage(
            tr("マルチカメラ EDL 適用 (V1/A1=%1 セグメント, %2 件スキップ)")
                .arg(v1Clips.size()).arg(skipped), 6000);
    } else {
        statusBar()->showMessage(
            tr("マルチカメラ EDL を V1/A1 に適用 (%1 セグメント)")
                .arg(v1Clips.size()), 4000);
    }
}

void MainWindow::openRenderQueueDialog()
{
    if (!m_renderQueueDialog) {
        m_renderQueueDialog = new RenderQueueDialog(this);
        if (m_timeline) {
            const qint64 totalUs =
                static_cast<qint64>(m_timeline->totalDuration() * 1000000.0);
            m_renderQueueDialog->setDefaultTimelineRange(0, totalUs);
        }
    }
    m_renderQueueDialog->show();
    m_renderQueueDialog->raise();
    m_renderQueueDialog->activateWindow();
}

void MainWindow::openSceneDetector()
{
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, tr("シーン検出"),
            tr("先にクリップを追加してください。"));
        return;
    }

    // Run the existing AutoEdit scene-change analyser (synchronous; the
    // SceneDetector class is the streaming variant and requires a frame
    // pump that is deferred). On each detected cut, drop a Timeline
    // marker at the timestamp.
    const auto &clips = m_timeline->videoClips();
    int targetIdx = m_timeline->selectedVideoClipIndex();
    if (targetIdx < 0 || targetIdx >= clips.size())
        targetIdx = 0;
    const ClipInfo &clip = clips[targetIdx];

    statusBar()->showMessage(tr("シーン変化を解析しています..."));
    QApplication::processEvents();

    auto scenes = AutoEdit::detectSceneChanges(clip.filePath);
    if (scenes.isEmpty()) {
        statusBar()->showMessage(tr("シーン変化が検出されませんでした"), 4000);
        return;
    }

    // Drop a Timeline marker at every detected cut.
    const QColor sceneCutColor("#3399ff");
    int added = 0;
    for (const auto &s : scenes) {
        const qint64 timeUs = static_cast<qint64>(s.time * 1000000.0);
        const QString label = tr("Scene Cut %1").arg(added + 1);
        m_timeline->addMarker(timeUs, label, sceneCutColor);
        ++added;
    }
    statusBar()->showMessage(
        tr("シーン検出: %1 個のカットにマーカーを追加しました").arg(added), 5000);
}

void MainWindow::runMotionStabilizer()
{
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, tr("スタビライズ"),
            tr("先にクリップを追加してください。"));
        return;
    }

    bool ok = false;
    int smoothPct = QInputDialog::getInt(this, tr("スタビライズ"),
        tr("Smoothness (1-100, higher=smoother):"),
        50, 1, 100, 1, &ok);
    if (!ok)
        return;

    // US-INT-4: synchronous analyse + bake. V1 clip 0 only for v1.
    const auto &clips = m_timeline->videoClips();
    const ClipInfo &target = clips.first();
    statusBar()->showMessage(tr("スタビライズ解析中..."), 0);
    QApplication::processEvents();

    MotionStabilizer stab;
    stab.setSmoothness(smoothPct / 100.0);
    QVector<StabilizerKeyframe> kfs = stab.analyzeFile(target.filePath);
    if (kfs.isEmpty()) {
        statusBar()->showMessage(
            tr("スタビライズ失敗: フレームを解析できませんでした"), 6000);
        return;
    }
    m_timeline->setClipStabilizerKeyframes(0, kfs);
    statusBar()->showMessage(
        tr("スタビライズ完了: %1 フレーム").arg(kfs.size()), 6000);
}

void MainWindow::addAdjustmentLayerCmd()
{
    if (!m_timeline) {
        QMessageBox::information(this, tr("調整レイヤー"),
            tr("タイムラインが利用できません。"));
        return;
    }

    AdjustmentLayer layer;
    // Use playhead as start; default 5-second duration so the layer is
    // visible in the timeline immediately.
    const qint64 startUs =
        static_cast<qint64>(m_timeline->playheadPosition() * 1000000.0);
    layer.timelineStartUs = startUs;
    layer.timelineEndUs = startUs + 5LL * 1000000LL;
    layer.trackIndex = 0;
    layer.name = tr("Adjustment Layer");

    // Seed grading from the current ColorGradingPanel state if visible
    // so the user gets a layer that already reflects what they see.
    if (m_colorGradingPanel) {
        const ColorWheels cw = m_colorGradingPanel->currentWheels();
        layer.lift[0]  = cw.lift.x();   layer.lift[1]  = cw.lift.y();
        layer.lift[2]  = cw.lift.z();   layer.lift[3]  = cw.liftLuma;
        layer.gamma[0] = cw.gamma.x();  layer.gamma[1] = cw.gamma.y();
        layer.gamma[2] = cw.gamma.z();  layer.gamma[3] = cw.gammaLuma;
        layer.gain[0]  = cw.gain.x();   layer.gain[1]  = cw.gain.y();
        layer.gain[2]  = cw.gain.z();   layer.gain[3]  = cw.gainLuma;
        layer.gradingEnabled = true;
    }

    const int newId = m_timeline->addAdjustmentLayer(layer);
    statusBar()->showMessage(
        tr("調整レイヤーを追加しました (id=%1, %2s..%3s)")
            .arg(newId)
            .arg(layer.timelineStartUs / 1.0e6, 0, 'f', 2)
            .arg(layer.timelineEndUs / 1.0e6, 0, 'f', 2),
        4000);
}

void MainWindow::openSpeedRampDialog()
{
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, tr("速度 / 持続時間"),
            tr("先にクリップを追加してください。"));
        return;
    }
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, tr("速度 / 持続時間"),
            tr("クリップを選択してください。"));
        return;
    }

    bool ok = false;
    const double speedMul = QInputDialog::getDouble(this, tr("速度 / 持続時間"),
        tr("速度倍率 (0.1 - 5.0):"), 1.0, 0.1, 5.0, 2, &ok);
    if (!ok)
        return;

    speedramp::SpeedRamp ramp;
    ramp.clearAndSetIdentity();
    ramp.addKeyframe(0, speedMul);

    const int clipIdx = m_timeline->selectedVideoClipIndex();
    if (clipIdx < 0) {
        QMessageBox::information(this, tr("速度 / 持続時間"),
            tr("V1 にクリップを選択してください。"));
        return;
    }
    m_timeline->setSpeedRamp(clipIdx, ramp);
    statusBar()->showMessage(
        tr("速度ランプを %1x に設定しました (clip #%2)")
            .arg(speedMul, 0, 'f', 2).arg(clipIdx), 5000);
}

void MainWindow::addQuickMarker()
{
    if (!m_timeline) return;
    const qint64 timeUs =
        static_cast<qint64>(m_timeline->playheadPosition() * 1000000.0);
    const int id = m_timeline->addMarker(timeUs, QStringLiteral("Marker"),
                                          QColor(QStringLiteral("#ff5050")));
    statusBar()->showMessage(
        tr("マーカー追加 (id=%1, %2s)")
            .arg(id)
            .arg(timeUs / 1.0e6, 0, 'f', 2), 3000);
}

void MainWindow::addColoredMarker()
{
    if (!m_timeline) return;
    QColor c = QColorDialog::getColor(QColor(QStringLiteral("#ff5050")),
                                      this, tr("マーカーの色を選択"));
    if (!c.isValid())
        return;

    bool ok = false;
    QString label = QInputDialog::getText(this, tr("色付きマーカー"),
        tr("ラベル (空でも可):"), QLineEdit::Normal, QString(), &ok);
    if (!ok)
        return;

    const qint64 timeUs =
        static_cast<qint64>(m_timeline->playheadPosition() * 1000000.0);
    if (label.isEmpty())
        label = QStringLiteral("Marker");
    const int id = m_timeline->addMarker(timeUs, label, c);
    statusBar()->showMessage(
        tr("色付きマーカー追加 (id=%1, %2s, %3)")
            .arg(id)
            .arg(timeUs / 1.0e6, 0, 'f', 2)
            .arg(c.name()), 3000);
}

void MainWindow::jumpToNextMarker()
{
    if (!m_timeline) return;
    const qint64 nowUs =
        static_cast<qint64>(m_timeline->playheadPosition() * 1000000.0);
    const int id = m_timeline->nextMarkerAfter(nowUs);
    if (id < 0) {
        statusBar()->showMessage(tr("これより後にマーカーがありません"), 2500);
        return;
    }
    const auto m = m_timeline->markerById(id);
    m_timeline->setPlayheadPosition(m.timelineUs / 1.0e6);
    statusBar()->showMessage(
        tr("マーカーへジャンプ: %1 (%2s)")
            .arg(m.label)
            .arg(m.timelineUs / 1.0e6, 0, 'f', 2), 2500);
}

void MainWindow::jumpToPrevMarker()
{
    if (!m_timeline) return;
    const qint64 nowUs =
        static_cast<qint64>(m_timeline->playheadPosition() * 1000000.0);
    const int id = m_timeline->prevMarkerBefore(nowUs);
    if (id < 0) {
        statusBar()->showMessage(tr("これより前にマーカーがありません"), 2500);
        return;
    }
    const auto m = m_timeline->markerById(id);
    m_timeline->setPlayheadPosition(m.timelineUs / 1.0e6);
    statusBar()->showMessage(
        tr("マーカーへジャンプ: %1 (%2s)")
            .arg(m.label)
            .arg(m.timelineUs / 1.0e6, 0, 'f', 2), 2500);
}

void MainWindow::openVoiceOverDialog()
{
    if (m_voiceOverDialog) {
        m_voiceOverDialog->raise();
        m_voiceOverDialog->activateWindow();
        return;
    }

    // Build default output path
    QString projectDir;
    if (!m_projectFilePath.isEmpty()) {
        projectDir = QFileInfo(m_projectFilePath).absolutePath();
    } else {
        projectDir = QDir::homePath();
    }
    QString audioDir = projectDir + "/audio";
    QDir dir(audioDir);
    if (!dir.exists())
        dir.mkpath(".");

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
    QString defaultPath = audioDir + "/voiceover-" + timestamp + ".wav";

    m_voiceOverDialog = new voiceover::VoiceOverDialog(defaultPath, this);

    // Mute audio preview during recording to prevent feedback
    const bool wasMuted = m_player ? m_player->isMuted() : false;
    if (m_player)
        m_player->setMuted(true);

    connect(m_voiceOverDialog, &voiceover::VoiceOverDialog::recordingStopped,
            this, [this](const QString &wavPath, qint64) {
                if (m_voiceOverDialog && m_voiceOverDialog->insertAtPlayhead() && m_timeline) {
                    m_timeline->insertAudioClipAtPlayhead(wavPath, 2);
                    statusBar()->showMessage(
                        tr("Voice-over inserted at playhead: %1").arg(wavPath), 5000);
                }
            });

    connect(m_voiceOverDialog, &QDialog::finished,
            this, [this, wasMuted](int) {
                if (m_player)
                    m_player->setMuted(wasMuted);
                m_voiceOverDialog->deleteLater();
                m_voiceOverDialog = nullptr;
            });

    m_voiceOverDialog->exec();
}
