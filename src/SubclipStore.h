#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class Timeline;
struct ClipInfo;

namespace subclips {

// Premiere Pro / DaVinci Resolve / Avid Subclip:
// Virtual trimmed [inPoint, outPoint] slice of a source media file (zero re-encoding).
struct SubclipEntry {
    QString id;
    QString name;
    QString sourcePath;
    double sourceDuration = 0.0;
    double inPoint = 0.0;
    double outPoint = 0.0;
    QString notes;

    double duration() const { return qMax(0.0, outPoint - inPoint); }
};

QVector<SubclipEntry> loadSubclips(const QString &overrideJsonPath = {});
bool saveSubclips(const QVector<SubclipEntry> &list, const QString &overrideJsonPath = {});
SubclipEntry createSubclip(const QString &name,
                           const QString &sourcePath,
                           double inPoint,
                           double outPoint,
                           double sourceDuration = 0.0,
                           const QString &notes = {},
                           const QString &overrideJsonPath = {});
SubclipEntry createSubclipFromClip(const ClipInfo &clip,
                                   const QString &customName = {},
                                   const QString &overrideJsonPath = {});
bool removeSubclip(const QString &id, const QString &overrideJsonPath = {});

// Insert a saved subclip onto Timeline video track `trackIdx` as a real trimmed ClipInfo
// with a single Undo step.
bool insertSubclipIntoTimeline(Timeline *timeline,
                               const SubclipEntry &entry,
                               int videoTrackIdx = 0);

class SubclipManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit SubclipManagerDialog(Timeline *timeline,
                                  QWidget *parent = nullptr,
                                  const QString &overrideJsonPath = {});

    int subclipCount() const;

private:
    void reloadList();
    void onCreateManual();
    void onInsertSelected();
    void onDeleteSelected();

    Timeline *m_timeline = nullptr;
    QString m_overridePath;
    QLineEdit *m_searchEdit = nullptr;
    QListWidget *m_list = nullptr;
    QVector<SubclipEntry> m_entries;
};

} // namespace subclips
