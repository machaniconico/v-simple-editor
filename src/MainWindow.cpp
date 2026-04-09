#include "MainWindow.h"
#include "VideoPlayer.h"
#include "Timeline.h"
#include "ExportDialog.h"
#include "UndoManager.h"
#include "OverlayDialogs.h"
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

    auto *addTransAction = insertMenu->addAction("Add T&ransition...");
    connect(addTransAction, &QAction::triggered, this, &MainWindow::addTransition);

    auto *addImageAction = insertMenu->addAction("Add &Image / Still...");
    connect(addImageAction, &QAction::triggered, this, &MainWindow::addImageOverlay);

    auto *addPipAction = insertMenu->addAction("Add &Picture in Picture...");
    connect(addPipAction, &QAction::triggered, this, &MainWindow::addPip);

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
    setWindowTitle(QString("V Editor Simple - %1 (%2 %3fps)")
        .arg(m_projectConfig.name)
        .arg(m_projectConfig.resolutionLabel())
        .arg(m_projectConfig.fps));
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

void MainWindow::addTextOverlay()
{
    TextOverlayDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        auto overlay = dialog.result();
        // TODO: Store overlay in project and render on preview
        statusBar()->showMessage(QString("Added text: \"%1\"").arg(overlay.text));
    }
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
