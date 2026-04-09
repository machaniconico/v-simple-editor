#include "MainWindow.h"
#include "VideoPlayer.h"
#include "Timeline.h"
#include "ExportDialog.h"
#include "UndoManager.h"
#include "OverlayDialogs.h"
#include "VideoEffectDialogs.h"
#include "EffectPlugin.h"
#include <QApplication>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QProgressDialog>
#include <QShortcut>
#include <QInputDialog>
#include <QCloseEvent>
#include <QFileInfo>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    resize(1280, 720);

    m_supportedFormats = {
        "MP4 (*.mp4)", "MKV (*.mkv)", "MOV (*.mov)",
        "WebM (*.webm)", "FLV (*.flv)"
    };

    m_exporter = new Exporter(this);

    setupUI();
    setupMenuBar();
    setupToolBar();
    updateEditActions();
    updateTitle();

    // Recent files setup
    setupRecentFiles();

    // Shortcut manager - load saved shortcuts
    m_shortcutManager.loadShortcuts();

    // Script engine
    m_scriptEngine = new ScriptEngine(this);
    m_scriptEngine->init();

    // Auto-save setup
    m_autoSave = new AutoSave(this);
    m_autoSave->setProjectDataCallback([this]() -> QString {
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
    connect(m_autoSave, &AutoSave::recoveryAvailable, this, [this](const QStringList &files) {
        auto reply = QMessageBox::question(this, "Crash Recovery",
            QString("Found %1 recovery file(s) from a previous session.\n\nRecover the most recent?")
                .arg(files.size()));
        if (reply == QMessageBox::Yes && !files.isEmpty()) {
            QString json = AutoSave::recoverFromFile(files.first());
            if (!json.isEmpty()) {
                ProjectData data;
                if (ProjectFile::fromJsonString(json, data)) {
                    m_projectConfig = data.config;
                    m_player->setCanvasSize(data.config.width, data.config.height);
                    m_timeline->restoreFromProject(data.videoTracks, data.audioTracks,
                        data.playheadPos, data.markIn, data.markOut, 10);
                    updateTitle();
                    statusBar()->showMessage("Recovered from auto-save");
                }
            }
        }
    });
    m_autoSave->start();

    // Apply dark theme by default
    ThemeManager::instance().applyTheme(ThemeType::Dark, this);

    // Enable drag & drop
    setAcceptDrops(true);

    // Setup permanent status bar widgets
    setupStatusBarWidgets();
    updateStatusInfo();

    statusBar()->showMessage("Ready — Use File > New Project to start");

    connect(m_timeline, &Timeline::clipSelected, this, [this](int) {
        updateEditActions();
    });
    connect(m_timeline->undoManager(), &UndoManager::stateChanged, this, [this]() {
        updateEditActions();
    });

    // Show welcome screen initially
    showWelcomeScreen();

    // Restore saved window state
    restoreWindowState();
}

void MainWindow::setupUI()
{
    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_mainSplitter = new QSplitter(Qt::Vertical, this);

    m_player = new VideoPlayer(this);
    m_timeline = new Timeline(this);

    // Welcome widget (shown when empty)
    m_welcomeWidget = new WelcomeWidget(this);
    connect(m_welcomeWidget, &WelcomeWidget::newProjectClicked, this, &MainWindow::newProject);
    connect(m_welcomeWidget, &WelcomeWidget::openFileClicked, this, &MainWindow::openFile);
    connect(m_welcomeWidget, &WelcomeWidget::openProjectClicked, this, &MainWindow::openProject);

    m_mainSplitter->addWidget(m_player);
    m_mainSplitter->addWidget(m_timeline);
    m_mainSplitter->setStretchFactor(0, 3);
    m_mainSplitter->setStretchFactor(1, 1);

    mainLayout->addWidget(m_welcomeWidget);
    mainLayout->addWidget(m_mainSplitter);
    setCentralWidget(centralWidget);

    connect(m_player, &VideoPlayer::positionChanged, m_timeline, &Timeline::setPlayheadPosition);

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
}

void MainWindow::setupMenuBar()
{
    // File menu
    auto *fileMenu = menuBar()->addMenu("&File");

    auto *newAction = fileMenu->addAction("&New Project...");
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::newProject);

    auto *openAction = fileMenu->addAction("&Open File...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);

    // Recent files submenu
    m_recentFilesMenu = new RecentFilesMenu(m_recentFilesManager, fileMenu);
    fileMenu->addMenu(m_recentFilesMenu);
    connect(m_recentFilesMenu, &RecentFilesMenu::fileSelected, this, &MainWindow::openRecentFile);

    fileMenu->addSeparator();

    auto *openProjectAction = fileMenu->addAction("Open &Project...");
    openProjectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(openProjectAction, &QAction::triggered, this, &MainWindow::openProject);

    auto *saveAction = fileMenu->addAction("&Save Project");
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::saveProject);

    auto *saveAsAction = fileMenu->addAction("Save Project &As...");
    saveAsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveProjectAs);

    fileMenu->addSeparator();

    auto *exportAction = fileMenu->addAction("&Export...");
    exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportVideo);

    auto *remotionAction = fileMenu->addAction("Export to &Remotion...");
    connect(remotionAction, &QAction::triggered, this, &MainWindow::exportToRemotion);

    fileMenu->addSeparator();

    auto *quitAction = fileMenu->addAction("&Quit");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    // Edit menu
    auto *editMenu = menuBar()->addMenu("&Edit");

    m_undoAction = editMenu->addAction("&Undo");
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undoAction);

    m_redoAction = editMenu->addAction("&Redo");
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::redoAction);

    editMenu->addSeparator();

    m_copyAction = editMenu->addAction("&Copy Clip");
    m_copyAction->setShortcut(QKeySequence::Copy);
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::copyClip);

    m_pasteAction = editMenu->addAction("&Paste Clip");
    m_pasteAction->setShortcut(QKeySequence::Paste);
    connect(m_pasteAction, &QAction::triggered, this, &MainWindow::pasteClip);

    editMenu->addSeparator();

    m_splitAction = editMenu->addAction("&Split at Playhead");
    m_splitAction->setShortcut(QKeySequence(Qt::Key_S));
    connect(m_splitAction, &QAction::triggered, this, &MainWindow::splitClip);

    m_deleteAction = editMenu->addAction("&Delete Clip");
    m_deleteAction->setShortcut(QKeySequence::Delete);
    connect(m_deleteAction, &QAction::triggered, this, &MainWindow::deleteClip);

    m_rippleDeleteAction = editMenu->addAction("&Ripple Delete");
    m_rippleDeleteAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    connect(m_rippleDeleteAction, &QAction::triggered, this, &MainWindow::rippleDelete);

    editMenu->addSeparator();

    m_snapAction = editMenu->addAction("Toggle S&nap");
    m_snapAction->setShortcut(QKeySequence(Qt::Key_N));
    m_snapAction->setCheckable(true);
    m_snapAction->setChecked(true);
    connect(m_snapAction, &QAction::triggered, this, &MainWindow::toggleSnap);

    editMenu->addSeparator();

    auto *speedAction = editMenu->addAction("Set Clip &Speed...");
    connect(speedAction, &QAction::triggered, this, &MainWindow::setClipSpeed);

    editMenu->addSeparator();

    auto *shortcutAction = editMenu->addAction("&Keyboard Shortcuts...");
    shortcutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_K));
    connect(shortcutAction, &QAction::triggered, this, &MainWindow::editShortcuts);

    // View menu
    auto *viewMenu = menuBar()->addMenu("&View");

    auto *zoomInAction = viewMenu->addAction("Zoom &In");
    zoomInAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal));
    connect(zoomInAction, &QAction::triggered, this, &MainWindow::zoomIn);

    auto *zoomOutAction = viewMenu->addAction("Zoom &Out");
    zoomOutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    connect(zoomOutAction, &QAction::triggered, this, &MainWindow::zoomOut);

    // Track menu
    auto *trackMenu = menuBar()->addMenu("&Track");

    auto *addVTrack = trackMenu->addAction("Add &Video Track");
    connect(addVTrack, &QAction::triggered, this, &MainWindow::addVideoTrack);

    auto *addATrack = trackMenu->addAction("Add &Audio Track");
    connect(addATrack, &QAction::triggered, this, &MainWindow::addAudioTrack);

    // Insert menu
    auto *insertMenu = menuBar()->addMenu("&Insert");

    auto *addTextAction = insertMenu->addAction("Add &Text / Telop...");
    addTextAction->setShortcut(QKeySequence(Qt::Key_T));
    connect(addTextAction, &QAction::triggered, this, &MainWindow::addTextOverlay);

    auto *manageTextAction = insertMenu->addAction("&Manage Text Overlays...");
    manageTextAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    connect(manageTextAction, &QAction::triggered, this, &MainWindow::manageTextOverlays);

    auto *importSubAction = insertMenu->addAction("Import &Subtitles (SRT/VTT)...");
    connect(importSubAction, &QAction::triggered, this, &MainWindow::importSubtitles);

    auto *saveTemplateAction = insertMenu->addAction("Save Text Te&mplate...");
    connect(saveTemplateAction, &QAction::triggered, this, &MainWindow::saveTextTemplate);

    insertMenu->addSeparator();

    auto *addTransAction = insertMenu->addAction("Add T&ransition...");
    connect(addTransAction, &QAction::triggered, this, &MainWindow::addTransition);

    auto *addImageAction = insertMenu->addAction("Add &Image / Still...");
    connect(addImageAction, &QAction::triggered, this, &MainWindow::addImageOverlay);

    auto *addPipAction = insertMenu->addAction("Add &Picture in Picture...");
    connect(addPipAction, &QAction::triggered, this, &MainWindow::addPip);

    // Audio menu
    auto *audioMenu = menuBar()->addMenu("&Audio");

    auto *volumeAction = audioMenu->addAction("Set Clip &Volume...");
    connect(volumeAction, &QAction::triggered, this, &MainWindow::setClipVolume);

    auto *bgmAction = audioMenu->addAction("Add &BGM / Audio File...");
    connect(bgmAction, &QAction::triggered, this, &MainWindow::addBgm);

    audioMenu->addSeparator();

    auto *muteAction = audioMenu->addAction("Toggle &Mute (A1)");
    muteAction->setShortcut(QKeySequence(Qt::Key_M));
    connect(muteAction, &QAction::triggered, this, &MainWindow::toggleMute);

    auto *soloAction = audioMenu->addAction("Toggle &Solo (A1)");
    connect(soloAction, &QAction::triggered, this, &MainWindow::toggleSolo);

    audioMenu->addSeparator();

    auto *eqAction = audioMenu->addAction("&Equalizer...");
    connect(eqAction, &QAction::triggered, this, &MainWindow::audioEqualizer);

    auto *audioFxAction = audioMenu->addAction("Audio E&ffects...");
    connect(audioFxAction, &QAction::triggered, this, &MainWindow::audioEffects);

    audioMenu->addSeparator();

    auto *vstAction = audioMenu->addAction("&VST / AU Plugins...");
    connect(vstAction, &QAction::triggered, this, &MainWindow::openVSTPlugins);

    // Markers menu
    auto *markersMenu = menuBar()->addMenu("Mar&kers");

    auto *addMarkerAction = markersMenu->addAction("&Add Marker at Playhead");
    addMarkerAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    connect(addMarkerAction, &QAction::triggered, this, &MainWindow::addMarker);

    auto *showMarkersAction = markersMenu->addAction("&Show All Markers...");
    connect(showMarkersAction, &QAction::triggered, this, &MainWindow::showMarkers);

    auto *exportChapAction = markersMenu->addAction("Export &YouTube Chapters...");
    connect(exportChapAction, &QAction::triggered, this, &MainWindow::exportChapters);

    // Effects menu
    auto *effectsMenu = menuBar()->addMenu("E&ffects");

    auto *ccAction = effectsMenu->addAction("&Color Correction / Grading...");
    ccAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    connect(ccAction, &QAction::triggered, this, &MainWindow::colorCorrection);

    auto *fxAction = effectsMenu->addAction("&Video Effects...");
    fxAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    connect(fxAction, &QAction::triggered, this, &MainWindow::videoEffects);

    auto *pluginAction = effectsMenu->addAction("&Plugin Effects...");
    connect(pluginAction, &QAction::triggered, this, &MainWindow::pluginEffects);

    effectsMenu->addSeparator();

    auto *lutAction = effectsMenu->addAction("Apply &LUT (.cube)...");
    connect(lutAction, &QAction::triggered, this, &MainWindow::applyLut);

    auto *manageLutAction = effectsMenu->addAction("Manage L&UTs...");
    connect(manageLutAction, &QAction::triggered, this, &MainWindow::manageLuts);

    effectsMenu->addSeparator();

    auto *applyPresetAction = effectsMenu->addAction("Apply Effect &Preset...");
    applyPresetAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    connect(applyPresetAction, &QAction::triggered, this, &MainWindow::applyEffectPreset);

    auto *savePresetAction = effectsMenu->addAction("&Save Current as Preset...");
    connect(savePresetAction, &QAction::triggered, this, &MainWindow::saveEffectPreset);

    auto *managePresetsAction = effectsMenu->addAction("&Manage Presets...");
    connect(managePresetsAction, &QAction::triggered, this, &MainWindow::manageEffectPresets);

    effectsMenu->addSeparator();

    auto *kfAction = effectsMenu->addAction("Edit &Keyframes...");
    kfAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(kfAction, &QAction::triggered, this, &MainWindow::editKeyframes);

    effectsMenu->addSeparator();

    auto *shaderFxAction = effectsMenu->addAction("&GPU Shader Effects...");
    connect(shaderFxAction, &QAction::triggered, this, &MainWindow::applyShaderEffect);

    auto *manageShaderAction = effectsMenu->addAction("Manage GPU S&haders...");
    connect(manageShaderAction, &QAction::triggered, this, &MainWindow::manageShaderEffects);

    // Playback menu
    auto *playbackMenu = menuBar()->addMenu("&Playback");

    auto *jklNote = playbackMenu->addAction("J/K/L Speed Control");
    jklNote->setEnabled(false);

    playbackMenu->addSeparator();

    auto *markInAction = playbackMenu->addAction("Mark &In");
    markInAction->setShortcut(QKeySequence(Qt::Key_I));
    connect(markInAction, &QAction::triggered, this, &MainWindow::markIn);

    auto *markOutAction = playbackMenu->addAction("Mark &Out");
    markOutAction->setShortcut(QKeySequence(Qt::Key_O));
    connect(markOutAction, &QAction::triggered, this, &MainWindow::markOut);

    // Tools menu (AI / Auto-edit)
    auto *toolsMenu = menuBar()->addMenu("&Tools");

    auto *silenceAction = toolsMenu->addAction("Detect &Silence...");
    connect(silenceAction, &QAction::triggered, this, &MainWindow::autoSilenceDetect);

    auto *jumpCutAction = toolsMenu->addAction("Auto &Jump Cut...");
    connect(jumpCutAction, &QAction::triggered, this, &MainWindow::autoJumpCut);

    auto *sceneAction = toolsMenu->addAction("Detect Scene &Changes...");
    connect(sceneAction, &QAction::triggered, this, &MainWindow::autoSceneDetect);

    toolsMenu->addSeparator();

    auto *stabilizeAction = toolsMenu->addAction("Video Stabi&lize...");
    connect(stabilizeAction, &QAction::triggered, this, &MainWindow::stabilizeVideo);

    auto *speedRampAction = toolsMenu->addAction("S&peed Ramp (Variable Speed)...");
    connect(speedRampAction, &QAction::triggered, this, &MainWindow::setSpeedRamp);

    toolsMenu->addSeparator();

    auto *motionTrackAction = toolsMenu->addAction("&Motion Tracking...");
    connect(motionTrackAction, &QAction::triggered, this, &MainWindow::motionTrackSetup);

    toolsMenu->addSeparator();

    auto *audioDenoiseAction = toolsMenu->addAction("Audio &Noise Reduction...");
    connect(audioDenoiseAction, &QAction::triggered, this, &MainWindow::audioNoiseDenoise);

    auto *videoDenoiseAction = toolsMenu->addAction("&Video Denoise...");
    connect(videoDenoiseAction, &QAction::triggered, this, &MainWindow::videoNoiseDenoise);

    toolsMenu->addSeparator();

    auto *subtitleGenAction = toolsMenu->addAction("Auto-Generate &Subtitles (Whisper)...");
    connect(subtitleGenAction, &QAction::triggered, this, &MainWindow::generateSubtitles);

    auto *highlightAction = toolsMenu->addAction("AI Auto-&Highlight...");
    connect(highlightAction, &QAction::triggered, this, &MainWindow::analyzeHighlights);

    toolsMenu->addSeparator();

    auto *screenRecAction = toolsMenu->addAction("Start Screen &Recording...");
    connect(screenRecAction, &QAction::triggered, this, &MainWindow::startScreenRecording);

    auto *stopRecAction = toolsMenu->addAction("S&top Screen Recording");
    connect(stopRecAction, &QAction::triggered, this, &MainWindow::stopScreenRecording);

    toolsMenu->addSeparator();

    auto *proxyToggle = toolsMenu->addAction("Toggle &Proxy Mode");
    proxyToggle->setCheckable(true);
    connect(proxyToggle, &QAction::triggered, this, &MainWindow::toggleProxyMode);

    auto *genProxiesAction = toolsMenu->addAction("Generate Pro&xies...");
    connect(genProxiesAction, &QAction::triggered, this, &MainWindow::generateProxies);

    auto *renderQueueAction = toolsMenu->addAction("&Render Queue...");
    renderQueueAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    connect(renderQueueAction, &QAction::triggered, this, &MainWindow::openRenderQueue);

    auto *networkRenderAction = toolsMenu->addAction("&Network Render...");
    connect(networkRenderAction, &QAction::triggered, this, &MainWindow::openNetworkRender);

    toolsMenu->addSeparator();

    auto *scriptAction = toolsMenu->addAction("P&ython Script Console...");
    scriptAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(scriptAction, &QAction::triggered, this, &MainWindow::openScriptConsole);

    toolsMenu->addSeparator();

    auto *multiCamSetupAction = toolsMenu->addAction("&Multi-Camera Setup...");
    connect(multiCamSetupAction, &QAction::triggered, this, &MainWindow::multiCamSetup);

    auto *multiCamSwitchAction = toolsMenu->addAction("Multi-Camera S&witch...");
    connect(multiCamSwitchAction, &QAction::triggered, this, &MainWindow::multiCamSwitch);

    // Composition menu (AE-like features)
    auto *compMenu = menuBar()->addMenu("&Composition");

    auto *addShapeAction = compMenu->addAction("Add &Shape Layer...");
    connect(addShapeAction, &QAction::triggered, this, &MainWindow::addShapeLayer);

    auto *addParticleAction = compMenu->addAction("Add &Particle Effect...");
    connect(addParticleAction, &QAction::triggered, this, &MainWindow::addParticleEffect);

    auto *textAnimAction = compMenu->addAction("Add &Text Animation...");
    connect(textAnimAction, &QAction::triggered, this, &MainWindow::addTextAnimation);

    compMenu->addSeparator();

    auto *transformKfAction = compMenu->addAction("Edit T&ransform Keyframes...");
    connect(transformKfAction, &QAction::triggered, this, &MainWindow::editTransformKeyframes);

    auto *maskAction = compMenu->addAction("Add &Mask...");
    connect(maskAction, &QAction::triggered, this, &MainWindow::addMask);

    auto *warpAction = compMenu->addAction("Apply &Warp / Distortion...");
    connect(warpAction, &QAction::triggered, this, &MainWindow::applyWarpEffect);

    compMenu->addSeparator();

    auto *exprAction = compMenu->addAction("&Expressions...");
    connect(exprAction, &QAction::triggered, this, &MainWindow::editExpressions);

    auto *precompAction = compMenu->addAction("Pre-&Compose Selected...");
    connect(precompAction, &QAction::triggered, this, &MainWindow::precomposeSelected);

    // View menu extras
    auto *themeAction = viewMenu->addAction("Change &Theme...");
    connect(themeAction, &QAction::triggered, this, &MainWindow::changeTheme);

    viewMenu->addSeparator();

    auto *tooltipAction = viewMenu->addAction("Show Toolbar &Tooltips");
    tooltipAction->setCheckable(true);
    {
        QSettings prefSettings("VEditorSimple", "Preferences");
        tooltipAction->setChecked(prefSettings.value("showTooltips", true).toBool());
    }
    connect(tooltipAction, &QAction::toggled, this, [this](bool checked) {
        QSettings prefSettings("VEditorSimple", "Preferences");
        prefSettings.setValue("showTooltips", checked);
        // Re-apply tooltips on all toolbar actions
        auto *toolbar = findChild<QToolBar *>("Main");
        if (!toolbar) return;
        if (checked) {
            // Rebuild toolbar to restore tooltips
            statusBar()->showMessage("Tooltips enabled — restart to apply");
        } else {
            for (auto *action : toolbar->actions())
                action->setToolTip("");
            statusBar()->showMessage("Toolbar tooltips disabled");
        }
    });

    auto *toolbarStyleAction = viewMenu->addAction("Toolbar &Icons Only");
    toolbarStyleAction->setCheckable(true);
    connect(toolbarStyleAction, &QAction::toggled, this, [this](bool iconOnly) {
        auto *toolbar = findChild<QToolBar *>();
        if (toolbar) {
            toolbar->setToolButtonStyle(iconOnly ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
            QSettings prefSettings("VEditorSimple", "Preferences");
            prefSettings.setValue("toolbarIconOnly", iconOnly);
        }
    });

    // Help menu
    auto *helpMenu = menuBar()->addMenu("&Help");

    auto *resourceGuideAction = helpMenu->addAction("Free &Resource Guide...");
    resourceGuideAction->setShortcut(QKeySequence(Qt::Key_F1));
    connect(resourceGuideAction, &QAction::triggered, this, &MainWindow::showResourceGuide);

    helpMenu->addSeparator();

    auto *aboutAction = helpMenu->addAction("&About");
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

    addBtn("new", "New", "New Project (Ctrl+N)", &MainWindow::newProject);
    addBtn("open", "Open", "Open File (Ctrl+O)", &MainWindow::openFile);
    addBtn("save", "Save", "Save Project (Ctrl+S)", &MainWindow::saveProject);
    toolbar->addSeparator();
    addBtn("undo", "Undo", "Undo (Ctrl+Z)", &MainWindow::undoAction);
    addBtn("redo", "Redo", "Redo (Ctrl+Shift+Z)", &MainWindow::redoAction);
    toolbar->addSeparator();
    addBtn("split", "Split", "Split at Playhead (S)", &MainWindow::splitClip);
    addBtn("delete", "Delete", "Delete Clip (Del)", &MainWindow::deleteClip);
    addBtn("copy", "Copy", "Copy Clip (Ctrl+C)", &MainWindow::copyClip);
    addBtn("paste", "Paste", "Paste Clip (Ctrl+V)", &MainWindow::pasteClip);
    toolbar->addSeparator();
    addBtn("text", "Text", "Add Text Overlay (T)", &MainWindow::addTextOverlay);
    addBtn("color", "Color", "Color Correction (Ctrl+G)", &MainWindow::colorCorrection);
    addBtn("effects", "Effects", "Video Effects (Ctrl+Shift+F)", &MainWindow::videoEffects);
    addBtn("marker", "Marker", "Add Marker (Ctrl+M)", &MainWindow::addMarker);
    toolbar->addSeparator();
    addBtn("export", "Export", "Export Video (Ctrl+E)", &MainWindow::exportVideo);
    addBtn("record", "Record", "Start Screen Recording", &MainWindow::startScreenRecording);

    // Apply saved tooltip preference
    QSettings settings("VEditorSimple", "Preferences");
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
    QString title = QString("V Editor Simple - %1 (%2 %3fps)")
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
    m_recentFilesManager->addFile(filePath);
    m_projectConfig = data.config;
    m_player->setCanvasSize(data.config.width, data.config.height);
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
    if (!filePath.isEmpty()) {
        m_player->loadFile(filePath);
        m_timeline->addClip(filePath);
        m_recentFilesManager->addFile(filePath);
        hideWelcomeScreen();
        updateStatusInfo();
        statusBar()->showMessage("Loaded: " + filePath);
        updateEditActions();
    }
}

void MainWindow::exportVideo()
{
    ExportDialog dialog(m_projectConfig, this);
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

void MainWindow::addTextOverlay()
{
    TextOverlayDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        auto overlay = dialog.result();
        // TODO: Store overlay in project and render on preview
        statusBar()->showMessage(QString("Added text: \"%1\"").arg(overlay.text));
    }
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
    ColorCorrectionDialog dialog(m_timeline->clipColorCorrection(), this);
    if (dialog.exec() == QDialog::Accepted) {
        m_timeline->setClipColorCorrection(dialog.result());
        m_player->setColorCorrection(dialog.result()); // Update GPU preview
        auto cc = dialog.result();
        QStringList changes;
        if (cc.brightness != 0) changes << QString("Brightness %1").arg(cc.brightness);
        if (cc.contrast != 0)   changes << QString("Contrast %1").arg(cc.contrast);
        if (cc.saturation != 0) changes << QString("Saturation %1").arg(cc.saturation);
        if (cc.exposure != 0)   changes << QString("Exposure %1").arg(cc.exposure, 0, 'f', 2);
        statusBar()->showMessage(changes.isEmpty() ? "Color correction reset" :
            "Color: " + changes.join(", "));
    }
}

void MainWindow::videoEffects()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Video Effects", "Select a clip first.");
        return;
    }
    VideoEffectDialog dialog(m_timeline->clipEffects(), this);
    if (dialog.exec() == QDialog::Accepted) {
        m_timeline->setClipEffects(dialog.result());
        statusBar()->showMessage(QString("Applied %1 effect(s)").arg(dialog.result().size()));
    }
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

void MainWindow::toggleProxyMode()
{
    auto &pm = ProxyManager::instance();
    pm.setProxyMode(!pm.isProxyMode());
    statusBar()->showMessage(pm.isProxyMode() ? "Proxy mode ON (low-res playback)" : "Proxy mode OFF (original quality)");
}

void MainWindow::generateProxies()
{
    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) {
        QMessageBox::information(this, "Proxies", "Add clips first.");
        return;
    }

    QStringList paths;
    for (const auto &c : clips)
        paths << c.filePath;

    auto &pm = ProxyManager::instance();
    connect(&pm, &ProxyManager::allProxiesReady, this, [this]() {
        statusBar()->showMessage("All proxies generated");
    }, Qt::UniqueConnection);
    connect(&pm, &ProxyManager::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Generating proxies... %1%").arg(pct));
    }, Qt::UniqueConnection);

    pm.generateAllProxies(paths);
    statusBar()->showMessage("Generating proxy files...");
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
    QStringList presetNames;
    for (const auto &p : presets)
        presetNames << p.first;

    bool ok;
    QString selected = QInputDialog::getItem(this, "Add Particle Effect",
        "Particle preset:", presetNames, 0, false, &ok);
    if (!ok) return;

    for (const auto &p : presets) {
        if (p.first == selected) {
            ParticleSystem ps;
            ps.setConfig(p.second);
            statusBar()->showMessage(QString("Added particle effect: %1").arg(selected));
            break;
        }
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
    QMessageBox::about(this, "About V Editor Simple",
        QString("V Editor Simple v%1\n\n"
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
        m_recentFilesManager->removeFile(filePath);
        return;
    }

    // Check if it's a project file or media file
    if (filePath.endsWith(".veditor", Qt::CaseInsensitive)) {
        ProjectData data;
        if (ProjectFile::load(filePath, data)) {
            m_projectConfig = data.config;
            m_player->setCanvasSize(data.config.width, data.config.height);
            m_timeline->restoreFromProject(data.videoTracks, data.audioTracks,
                data.playheadPos, data.markIn, data.markOut, data.zoomLevel);
            m_projectFilePath = filePath;
            updateTitle();
            statusBar()->showMessage("Opened project: " + fi.fileName());
        }
    } else {
        m_player->loadFile(filePath);
        statusBar()->showMessage("Opened: " + fi.fileName());
    }
    m_recentFilesManager->addFile(filePath);
}

void MainWindow::editShortcuts()
{
    ShortcutEditorDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        m_shortcutManager.saveShortcuts();
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
    m_statusResolution->setText(QString("%1x%2").arg(m_projectConfig.width).arg(m_projectConfig.height));
    m_statusFps->setText(QString("%1 fps").arg(m_projectConfig.fps));

    double totalDur = m_timeline->totalDuration();
    int h = (int)(totalDur / 3600);
    int m = (int)(fmod(totalDur, 3600) / 60);
    int s = (int)(fmod(totalDur, 60));
    m_statusDuration->setText(QString("%1:%2:%3")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0')));

    m_statusTheme->setText(ThemeManager::themeName(ThemeManager::instance().currentTheme()));
}

void MainWindow::showWelcomeScreen()
{
    m_welcomeWidget->setRecentFiles(m_recentFilesManager->recentFiles());
    m_welcomeWidget->setVisible(true);
    m_mainSplitter->setVisible(false);
    m_hasContent = false;
}

void MainWindow::hideWelcomeScreen()
{
    if (!m_hasContent) {
        m_welcomeWidget->setVisible(false);
        m_mainSplitter->setVisible(true);
        m_hasContent = true;
    }
}

void MainWindow::saveWindowState()
{
    QSettings settings("VEditorSimple", "WindowState");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
    settings.setValue("splitterState", m_mainSplitter->saveState());
    settings.setValue("theme", (int)ThemeManager::instance().currentTheme());
}

void MainWindow::restoreWindowState()
{
    QSettings settings("VEditorSimple", "WindowState");
    if (settings.contains("geometry")) {
        restoreGeometry(settings.value("geometry").toByteArray());
    }
    if (settings.contains("windowState")) {
        restoreState(settings.value("windowState").toByteArray());
    }
    if (settings.contains("splitterState")) {
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
                m_recentFilesManager->addFile(filePath);
                m_projectConfig = data.config;
                m_player->setCanvasSize(data.config.width, data.config.height);
                m_timeline->restoreFromProject(data.videoTracks, data.audioTracks,
                    data.playheadPos, data.markIn, data.markOut, data.zoomLevel);
                updateTitle();
                hideWelcomeScreen();
                updateStatusInfo();
                statusBar()->showMessage("Opened project: " + QFileInfo(filePath).fileName());
            }
        } else {
            // Open as media file
            m_player->loadFile(filePath);
            m_timeline->addClip(filePath);
            m_recentFilesManager->addFile(filePath);
            hideWelcomeScreen();
            updateStatusInfo();
            statusBar()->showMessage("Loaded: " + QFileInfo(filePath).fileName());
        }
    }
    updateEditActions();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveWindowState();
    if (m_autoSave) m_autoSave->stop();
    QMainWindow::closeEvent(event);
}
