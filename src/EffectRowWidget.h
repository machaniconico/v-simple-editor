#pragma once

#include <QWidget>
#include <QVector>
#include <QVariant>
#include "VideoEffect.h"

class QDoubleSpinBox;
class QSlider;
class QCheckBox;
class QPushButton;
class QComboBox;
class QLabel;
class QVBoxLayout;
class KeyframeTrack;

namespace effectctrl {

class EffectKeyframeToggle;
class EffectKeyframeNavBar;
struct ParamDef;

class EffectRowWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EffectRowWidget(QWidget *parent = nullptr);

    void setEffect(const VideoEffect &effect);
    VideoEffect currentEffect() const;

    void setParamHasTrack(const QString &paramName, bool has);
    bool paramHasTrack(const QString &paramName) const;
    double getParamValueByName(const QString &paramName) const;
    void setParamKeyframeTrack(const QString &paramName, KeyframeTrack *track,
                               double clipDurationSeconds, double playheadSeconds);
    void setPlayhead(double seconds);

signals:
    void paramChanged(const QString &paramName, const QVariant &newValue);
    void keyframeToggled(const QString &paramName, bool now);
    void keyframeTrackChanged(const QString &paramName);

private:
    void buildRows(const QVector<ParamDef> &schema);
    void clearRows();

    VideoEffect m_effect;

    struct RowWidgets {
        QString paramName;
        QWidget *container = nullptr;
        QVBoxLayout *containerLayout = nullptr;
        QLabel *label = nullptr;
        EffectKeyframeToggle *kfToggle = nullptr;
        QDoubleSpinBox *spinBox = nullptr;
        QSlider *slider = nullptr;
        QCheckBox *checkBox = nullptr;
        QPushButton *colorButton = nullptr;
        QComboBox *comboBox = nullptr;
        QPushButton *resetButton = nullptr;
        EffectKeyframeNavBar *navBar = nullptr;
        double defaultVal = 0.0;
    };

    QVector<RowWidgets> m_rows;

    double getParamValue(int rowIdx) const;
};

} // namespace effectctrl
