#pragma once

#include <QMainWindow>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QSplitter>
#include <QFileDialog>
#include "ProjectSettings.h"

class VideoPlayer;
class Timeline;

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
};
