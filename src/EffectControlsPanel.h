#pragma once

#include <QDockWidget>
#include <QLabel>
#include <QScrollArea>
#include <QGroupBox>
#include <QVector>
#include <QWidget>
#include <QVariant>
#include "VideoEffect.h"
#include "MotionSectionWidget.h"

class Timeline;
class MainWindow;

namespace effectctrl {
class EffectRowWidget;
class EffectControlsPanelToolbar;
class EffectControlsRowToolbar;

struct ClipKey {
    int trackIdx = -1;
    int clipIdx = -1;
    bool valid() const { return trackIdx >= 0 && clipIdx >= 0; }
};

class EffectControlsPanel : public QDockWidget
{
    Q_OBJECT

public:
    explicit EffectControlsPanel(QWidget *parent = nullptr);

    void setTimeline(Timeline *timeline);
    void setMainWindow(MainWindow *mw);
    void refreshFromCurrentClip();

signals:
    void effectsChanged(const QVector<VideoEffect> &effects);
    void motionChanged(const effectctrl::MotionState &motion);

private slots:
    void onParamChanged(int effectIndex, const QString &paramName, const QVariant &newValue);
    void onBypassToggled(int idx, bool enabled);
    void onResetRequested(int idx);
    void onDuplicateRequested(int idx);
    void onRemoveRequested(int idx);
    void onMoveUpRequested(int idx);
    void onMoveDownRequested(int idx);
    void onAddEffectRequested(VideoEffectType type);
    void onEffectKeyframeToggled(int effectIndex, const QString &paramName, bool now);
    void onMotionKeyframeToggled(const QString &propPath, bool now);

private:
    void buildEmptyState(const QString &message = QStringLiteral("No clip selected"));
    void buildEffectGroups(const QVector<VideoEffect> &effects);
    void clearContent();
    void rebuildRowToolbars();
    void setPlayheadOnVisibleNavBars(double seconds);
    void persistEditedKeyframes();
    double currentPlayheadSeconds() const;
    void createKeyframeTrack(const QString &trackName, double value);
    void removeKeyframeTrack(const QString &trackName, const QString &displayName);
    void persistAndRebuild();

    Timeline *m_timeline = nullptr;
    MainWindow *m_mainWindow = nullptr;
    ClipKey m_currentClipKey;
    QVector<VideoEffect> m_effects;
    MotionSectionWidget *m_motionWidget = nullptr;

    QLabel *m_emptyStateLabel = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_scrollContent = nullptr;
    QVBoxLayout *m_scrollLayout = nullptr;

    EffectControlsPanelToolbar *m_panelToolbar = nullptr;
    QVector<QGroupBox *> m_effectGroupBoxes;
    QVector<EffectControlsRowToolbar *> m_rowToolbars;
    QVector<EffectRowWidget *> m_effectRowWidgets;
};

} // namespace effectctrl
