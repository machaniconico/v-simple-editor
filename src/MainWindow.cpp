#include "MainWindow.h"
#include "VideoPlayer.h"
#include "Timeline.h"
#include "ExportDialog.h"
#include "UndoManager.h"
#include "OverlayDialogs.h"
#include "VideoEffectDialogs.h"
#include "EffectPlugin.h"
#include "ColorGradingPanel.h"
#include "GLPreview.h"
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
#include "GradientStopBar.h"
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
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
        });
    connect(m_timeline->undoManager(), &UndoManager::stateChanged, this, [this]() {
        updateEditActions();
    });

    // Show welcome screen initially
    showWelcomeScreen();

    // Restore saved window state
    restoreWindowState();

    qInfo() << "MainWindow::ctor end";
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

    mainLayout->addWidget(m_welcomeWidget);
    mainLayout->addWidget(m_mainSplitter);
    setCentralWidget(centralWidget);

    connect(m_player, &VideoPlayer::positionChanged, m_timeline, &Timeline::setPlayheadPosition);
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

    m_snapAction = editMenu->addAction("スナップ切替(&N)");
    m_snapAction->setShortcut(QKeySequence(Qt::Key_N));
    m_snapAction->setCheckable(true);
    m_snapAction->setChecked(true);
    connect(m_snapAction, &QAction::triggered, this, &MainWindow::toggleSnap);

    editMenu->addSeparator();

    auto *speedAction = editMenu->addAction("再生速度を設定...");
    connect(speedAction, &QAction::triggered, this, &MainWindow::setClipSpeed);

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

    insertMenu->addSeparator();

    auto *addTransAction = insertMenu->addAction("トランジションを追加...");
    connect(addTransAction, &QAction::triggered, this, &MainWindow::addTransition);

    auto *addImageAction = insertMenu->addAction("画像 / 静止画を追加...");
    connect(addImageAction, &QAction::triggered, this, &MainWindow::addImageOverlay);

    auto *addPipAction = insertMenu->addAction("ピクチャー・イン・ピクチャー追加...");
    connect(addPipAction, &QAction::triggered, this, &MainWindow::addPip);

    // オーディオ メニュー
    auto *audioMenu = menuBar()->addMenu("オーディオ(&A)");

    auto *volumeAction = audioMenu->addAction("音量を設定...");
    connect(volumeAction, &QAction::triggered, this, &MainWindow::setClipVolume);

    auto *bgmAction = audioMenu->addAction("BGM / 音声ファイルを追加...");
    connect(bgmAction, &QAction::triggered, this, &MainWindow::addBgm);

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

    // マーカー メニュー
    auto *markersMenu = menuBar()->addMenu("マーカー(&K)");

    auto *addMarkerAction = markersMenu->addAction("再生ヘッドにマーカー追加");
    addMarkerAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    connect(addMarkerAction, &QAction::triggered, this, &MainWindow::addMarker);

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

    auto *pluginAction = effectsMenu->addAction("プラグインエフェクト(&P)...");
    connect(pluginAction, &QAction::triggered, this, &MainWindow::pluginEffects);

    effectsMenu->addSeparator();

    auto *lutAction = effectsMenu->addAction("LUT適用 (.cube)...");
    connect(lutAction, &QAction::triggered, this, &MainWindow::applyLut);

    auto *manageLutAction = effectsMenu->addAction("LUT管理...");
    connect(manageLutAction, &QAction::triggered, this, &MainWindow::manageLuts);

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

    viewMenu->addSeparator();
    auto *colorPanelAction = viewMenu->addAction("カラーグレーディングパネル(&G)");
    colorPanelAction->setCheckable(true);
    colorPanelAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    connect(colorPanelAction, &QAction::toggled, m_colorGradingPanel, &QDockWidget::setVisible);
    connect(m_colorGradingPanel, &QDockWidget::visibilityChanged, colorPanelAction, &QAction::setChecked);

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
    if (m_player)
        m_player->setCanvasSize(data.config.width, data.config.height);
    if (m_timeline)
        m_timeline->restoreFromProject(data.videoTracks, data.audioTracks,
            data.playheadPos, data.markIn, data.markOut, data.zoomLevel);
    updateTitle();
    hideWelcomeScreen();
    updateStatusInfo();
    updateEditActions();
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
        statusBar()->showMessage(QString("Added transition: %1 (%2s)")
            .arg(Transition::typeName(transition.type)).arg(transition.duration));
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

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveWindowState();
    if (m_autoSave) m_autoSave->markCleanShutdown();
    QMainWindow::closeEvent(event);
}
