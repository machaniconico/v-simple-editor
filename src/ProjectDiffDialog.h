#pragma once

#include "ProjectDiff.h"
#include <QDialog>

class ProjectDiffDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProjectDiffDialog(const QVector<projdiff::Change> &changes, QWidget *parent = nullptr);

signals:
    void clipActivated(bool audio, int trackIndex, int clipIndex);
};
