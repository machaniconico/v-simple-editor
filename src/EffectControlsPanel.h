#pragma once

#include <QDockWidget>
#include <QLabel>
#include <QScrollArea>
#include <QGroupBox>
#include <QVector>
#include <QWidget>
#include <QVariant>
#include <QColor>
#include "VideoEffect.h"
#include "MotionSectionWidget.h"
#include "LayerStyle.h"

class Timeline;
class MainWindow;
class QCheckBox;
class QDoubleSpinBox;
class QPushButton;
class QSlider;
class QSpinBox;

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
    double currentClipLocalPlayheadSeconds() const;
    void createKeyframeTrack(const QString &trackName, double value);
    void removeKeyframeTrack(const QString &trackName, const QString &displayName);
    void persistAndRebuild();
    void buildLayerStyleGroup();
    void setLayerStyleControls(const LayerStyle &style, bool hasClip);
    LayerStyle layerStyleFromControls() const;
    void updateLayerStyleControlAvailability(bool hasClip);
    void updateLayerStyleColorButton(QPushButton *button, const QColor &color);
    void persistLayerStyleFromControls();
    void chooseShadowColor();
    void chooseStrokeColor();

    Timeline *m_timeline = nullptr;
    MainWindow *m_mainWindow = nullptr;
    ClipKey m_currentClipKey;
    QVector<VideoEffect> m_effects;
    MotionSectionWidget *m_motionWidget = nullptr;
    QGroupBox *m_layerStyleGroup = nullptr;
    QCheckBox *m_shadowEnable = nullptr;
    QPushButton *m_shadowColorButton = nullptr;
    QDoubleSpinBox *m_shadowOffsetX = nullptr;
    QDoubleSpinBox *m_shadowOffsetY = nullptr;
    QDoubleSpinBox *m_shadowBlurRadius = nullptr;
    QSlider *m_shadowOpacitySlider = nullptr;
    QSpinBox *m_shadowOpacitySpin = nullptr;
    QCheckBox *m_strokeEnable = nullptr;
    QPushButton *m_strokeColorButton = nullptr;
    QDoubleSpinBox *m_strokeWidth = nullptr;
    QColor m_shadowColor = QColor(0, 0, 0, 128);
    QColor m_strokeColor = QColor(Qt::white);
    bool m_blockLayerStyleUi = false;

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
