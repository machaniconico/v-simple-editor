#pragma once

#include "VideoEffect.h"

#include <QDialog>
#include <QString>
#include <QVector>

class QListWidget;
class Timeline;

namespace gradeversions {

struct GradeVersion {
    QString id;
    QString name;
    ColorCorrection grade;
    QString lutPath;
};

// Serialize/deserialize full ColorCorrection state cleanly.
QJsonObject gradeToJson(const ColorCorrection &cc);
ColorCorrection gradeFromJson(const QJsonObject &obj);

// Per-clip UID Local Grade Version store persisted in QSettings (DaVinci Resolve Local Versions).
QVector<GradeVersion> versionsForClip(const QString &clipUid);
GradeVersion saveVersion(const QString &clipUid,
                         const QString &name,
                         const ColorCorrection &grade,
                         const QString &lutPath = {});
bool deleteVersion(const QString &clipUid, const QString &versionId);
void clearVersionsForClip(const QString &clipUid);

// Apply a saved version onto Timeline's (trackIdx, clipIdx) with a single Undo step.
bool applyVersionToClip(Timeline *timeline,
                        int trackIdx,
                        int clipIdx,
                        const QString &versionId);

class GradeVersionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit GradeVersionsDialog(Timeline *timeline,
                                 int trackIdx,
                                 int clipIdx,
                                 QWidget *parent = nullptr);

private:
    void reloadList();
    void onSaveCurrent();
    void onApplySelected();
    void onDeleteSelected();

    Timeline *m_timeline = nullptr;
    int m_trackIdx = 0;
    int m_clipIdx = 0;
    QString m_clipUid;
    QListWidget *m_list = nullptr;
};

} // namespace gradeversions
