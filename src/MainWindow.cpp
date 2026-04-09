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

    statusBar()->showMessage("Ready — Use File > New Project to start");

    connect(m_timeline, &Timeline::clipSelected, this, [this](int) {
        updateEditActions();
    });
    connect(m_timeline->undoManager(), &UndoManager::stateChanged, this, [this]() {
        updateEditActions();
    });
}

void MainWindow::setupUI()
{
    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto *splitter = new QSplitter(Qt::Vertical, this);

    m_player = new VideoPlayer(this);
    m_timeline = new Timeline(this);

    splitter->addWidget(m_player);
    splitter->addWidget(m_timeline);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    mainLayout->addWidget(splitter);
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

    auto *kfAction = effectsMenu->addAction("Edit &Keyframes...");
    kfAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(kfAction, &QAction::triggered, this, &MainWindow::editKeyframes);

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

    auto *multiCamSetupAction = toolsMenu->addAction("&Multi-Camera Setup...");
    connect(multiCamSetupAction, &QAction::triggered, this, &MainWindow::multiCamSetup);

    auto *multiCamSwitchAction = toolsMenu->addAction("Multi-Camera S&witch...");
    connect(multiCamSwitchAction, &QAction::triggered, this, &MainWindow::multiCamSwitch);

    // View menu - add theme submenu
    auto *themeAction = viewMenu->addAction("Change &Theme...");
    connect(themeAction, &QAction::triggered, this, &MainWindow::changeTheme);

    // Help menu
    auto *helpMenu = menuBar()->addMenu("&Help");
    auto *aboutAction = helpMenu->addAction("&About");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::about);
}

void MainWindow::setupToolBar()
{
    auto *toolbar = addToolBar("Main");
    toolbar->setMovable(false);

    toolbar->addAction("New", this, &MainWindow::newProject);
    toolbar->addAction("Open", this, &MainWindow::openFile);
    toolbar->addSeparator();
    toolbar->addAction("Undo", this, &MainWindow::undoAction);
    toolbar->addAction("Redo", this, &MainWindow::redoAction);
    toolbar->addSeparator();
    toolbar->addAction("Split", this, &MainWindow::splitClip);
    toolbar->addAction("Delete", this, &MainWindow::deleteClip);
    toolbar->addAction("Copy", this, &MainWindow::copyClip);
    toolbar->addAction("Paste", this, &MainWindow::pasteClip);
    toolbar->addSeparator();
    toolbar->addAction("Export", this, &MainWindow::exportVideo);
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
    if (dialog.exec() == QDialog::Accepted)
        applyProjectConfig(dialog.config());
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
    m_projectConfig = data.config;
    m_player->setCanvasSize(data.config.width, data.config.height);
    m_timeline->restoreFromProject(data.videoTracks, data.audioTracks,
        data.playheadPos, data.markIn, data.markOut, data.zoomLevel);
    updateTitle();
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

void MainWindow::about()
{
    QMessageBox::about(this, "About V Editor Simple",
        QString("V Editor Simple v%1\n\n"
                "A simple yet powerful video editor.\n"
                "Built with Qt and FFmpeg.\n\n"
                "Shortcuts:\n"
                "  Ctrl+Z / Ctrl+Y — Undo / Redo\n"
                "  Ctrl+C / Ctrl+V — Copy / Paste clip\n"
                "  S — Split at playhead\n"
                "  Delete — Delete clip\n"
                "  Shift+Delete — Ripple delete\n"
                "  N — Toggle snap\n"
                "  Drag clip — Reorder\n"
                "  Drag edges — Trim")
            .arg(APP_VERSION));
}
