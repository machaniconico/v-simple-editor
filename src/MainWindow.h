#pragma once

#include <QMainWindow>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QSplitter>
#include <QFileDialog>
#include "ProjectSettings.h"
#include "Exporter.h"

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
};
