#include "MainWindow.h"
#include "VideoPlayer.h"
#include "Timeline.h"
#include <QApplication>
#include <QMessageBox>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    resize(1280, 720);

    m_supportedFormats = {
        "MP4 (*.mp4)",
        "MKV (*.mkv)",
        "MOV (*.mov)",
        "WebM (*.webm)",
        "FLV (*.flv)"
    };

    setupUI();
    setupMenuBar();
    setupToolBar();
    updateEditActions();
    updateTitle();

    statusBar()->showMessage("Ready — Use File > New Project to start");

    connect(m_timeline, &Timeline::clipSelected, this, [this](int) {
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

    m_splitAction = editMenu->addAction("&Split at Playhead");
    m_splitAction->setShortcut(QKeySequence(Qt::Key_S));
    connect(m_splitAction, &QAction::triggered, this, &MainWindow::splitClip);

    m_deleteAction = editMenu->addAction("&Delete Clip");
    m_deleteAction->setShortcut(QKeySequence::Delete);
    connect(m_deleteAction, &QAction::triggered, this, &MainWindow::deleteClip);

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
    toolbar->addAction("Split", this, &MainWindow::splitClip);
    toolbar->addAction("Delete", this, &MainWindow::deleteClip);
    toolbar->addSeparator();
    toolbar->addAction("Export", this, &MainWindow::exportVideo);
}

void MainWindow::updateEditActions()
{
    bool hasSel = m_timeline->hasSelection();
    m_deleteAction->setEnabled(hasSel);
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
        .arg(config.name)
        .arg(config.resolutionLabel())
        .arg(config.fps));
}

void MainWindow::newProject()
{
    ProjectSettingsDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        applyProjectConfig(dialog.config());
    }
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
    QString filePath = QFileDialog::getSaveFileName(this, "Export Video", QString(),
        "MP4 - H.264 (*.mp4);;"
        "MKV - H.265 (*.mkv);;"
        "WebM - VP9 (*.webm);;"
        "MP4 - AV1 (*.mp4)");
    if (!filePath.isEmpty()) {
        statusBar()->showMessage("Exporting: " + filePath);
        // TODO: Implement export via FFmpeg using m_projectConfig
    }
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

void MainWindow::about()
{
    QMessageBox::about(this, "About V Editor Simple",
        QString("V Editor Simple v%1\n\n"
                "A simple yet powerful video editor.\n"
                "Built with Qt and FFmpeg.\n\n"
                "Supported codecs: H.264, H.265, AV1\n"
                "Containers: MP4, MKV, MOV, WebM, FLV\n\n"
                "Export presets:\n"
                "  YouTube, YouTube Shorts, TikTok/Reels,\n"
                "  X/Twitter, Facebook, Twitch, Discord,\n"
                "  Niconico, Instagram Square\n\n"
                "Shortcuts:\n"
                "  Ctrl+N - New project\n"
                "  S - Split at playhead\n"
                "  Delete - Delete selected clip\n"
                "  Drag clip edges to trim")
            .arg(APP_VERSION));
}
