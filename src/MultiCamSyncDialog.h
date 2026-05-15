#pragma once

// MultiCamSyncDialog
// ------------------
// Sprint 21 US-MCAM-1. Audio-sync multi-camera dialog built on top of
// multicam::MultiCamSync. The user loads N camera video files, runs
// "Sync By Audio" (envelope cross-correlation against camera[0]), drops
// angle cuts (time -> cam index), and exports a switched EDL.
//
// This dialog is intentionally STANDALONE and modeless (setModal(false)).
// It owns its multicam::MultiCamSync as a child QObject and never touches
// Timeline / AudioMixer / decoders directly.
//
// NOTE: the spec'd public name was "MultiCamDialog", but an unrelated
// MultiCamDialog (MultiCamProject / QMediaPlayer based) already exists in
// the tree and is wired into the build + MainWindow. To avoid an ODR /
// duplicate-class collision without editing any existing file, this new
// dialog is delivered as MultiCamSyncDialog. The later CMake-integration
// story can wire it in under this name.

#include <QDialog>

#include "MultiCamSync.h"

class QTableWidget;
class QProgressBar;
class QPushButton;

class MultiCamSyncDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MultiCamSyncDialog(QWidget *parent = nullptr);

private slots:
    void onAddCamClicked();
    void onSyncClicked();
    void onAddCutClicked();
    void onExportEdlClicked();
    void onSyncProgress(int percent);
    void onSyncFinished();

private:
    void rebuildCamTable();
    void rebuildCutTable();

    multicam::MultiCamSync *m_sync     = nullptr;
    QTableWidget           *m_camTable = nullptr;
    QTableWidget           *m_cutTable = nullptr;
    QProgressBar           *m_progress = nullptr;
};
