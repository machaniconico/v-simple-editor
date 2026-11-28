#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QTableWidget;
class Timeline;

namespace tlindex {

struct IndexRow {
    enum class Kind { VideoClip, AudioClip, Marker };
    Kind kind = Kind::VideoClip;
    int trackIdx = 0;
    int clipIdx = -1;
    QString uid;
    QString name;
    QString trackLabel;
    QString roleName;
    double startSec = 0.0;
    double durationSec = 0.0;
    bool offline = false;
    bool duplicated = false;
};

struct FilterOptions {
    QString query;
    int kindFilter = 0;       // 0=All, 1=Video, 2=Audio, 3=Markers
    QString roleFilter;       // empty = all roles, or role id
    bool onlyOffline = false;
    bool onlyDuplicates = false;
};

// Build searchable/filterable index of every video clip, audio clip, and marker in Timeline
// (Final Cut Pro / DaVinci Resolve Timeline Index equivalent).
QVector<IndexRow> buildIndexRows(const Timeline *timeline, const FilterOptions &filter = {});

class TimelineIndexDialog : public QDialog {
    Q_OBJECT
public:
    explicit TimelineIndexDialog(Timeline *timeline, QWidget *parent = nullptr);

    void refresh();
    int rowCount() const;

signals:
    void jumpToTimeRequested(double timelineSec);

private:
    void onRowActivated(int row, int column);

    Timeline *m_timeline = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_kindCombo = nullptr;
    QComboBox *m_roleCombo = nullptr;
    QCheckBox *m_offlineOnly = nullptr;
    QCheckBox *m_dupOnly = nullptr;
    QTableWidget *m_table = nullptr;
    QVector<IndexRow> m_rows;
};

} // namespace tlindex
