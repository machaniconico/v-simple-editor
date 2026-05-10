#pragma once

#include <QWidget>
#include <QVector>

class QDoubleSpinBox;
class QFrame;
class QToolButton;
class QLabel;
class QVBoxLayout;
class QWidget;
class KeyframeTrack;

namespace effectctrl {

class EffectKeyframeToggle;
class EffectKeyframeNavBar;

struct MotionState {
    double scale = 1.0;
    double dx = 0.0;
    double dy = 0.0;
    double rotation2DDeg = 0.0;
    double opacity = 1.0;
    bool is3DLayer = false;
    double posZ = 0.0;
    double rotX = 0.0;
    double rotY = 0.0;
    double rotZ = 0.0;
};

class MotionSectionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MotionSectionWidget(QWidget *parent = nullptr);

    void setMotion(const MotionState &motion);
    MotionState currentMotion() const;

    void setPropHasTrack(const QString &propPath, bool has);
    bool propHasTrack(const QString &propPath) const;
    double getMotionValue(const QString &propPath) const;
    void setPropKeyframeTrack(const QString &propPath, KeyframeTrack *track,
                             double clipDurationSeconds, double playheadSeconds);
    void setPlayhead(double seconds);

signals:
    void motionChanged(const effectctrl::MotionState &motion);
    void keyframeToggled(const QString &propPath, bool now);
    void keyframeTrackChanged(const QString &propPath);

private:
    QDoubleSpinBox *createSpinBox(double min, double max, double step,
                                  int decimals, double value);
    void emitMotionChanged();
    void sync3DVisibility();
    void bindMotionRow(EffectKeyframeToggle *toggle, QDoubleSpinBox *spin, const QString &propPath);

    struct MotionRowWidgets {
        QString propPath;
        QWidget *container = nullptr;
        QVBoxLayout *containerLayout = nullptr;
        EffectKeyframeToggle *toggle = nullptr;
        QDoubleSpinBox *spin = nullptr;
        EffectKeyframeNavBar *navBar = nullptr;
    };

    MotionRowWidgets *findMotionRow(const QString &propPath);
    const MotionRowWidgets *findMotionRow(const QString &propPath) const;

    QToolButton *m_toggle3D = nullptr;
    QFrame *m_group3D = nullptr;

    EffectKeyframeToggle *m_posXToggle = nullptr;
    EffectKeyframeToggle *m_posYToggle = nullptr;
    EffectKeyframeToggle *m_scaleToggle = nullptr;
    EffectKeyframeToggle *m_rotation2DToggle = nullptr;
    EffectKeyframeToggle *m_opacityToggle = nullptr;

    QDoubleSpinBox *m_posXSpin = nullptr;
    QDoubleSpinBox *m_posYSpin = nullptr;
    QDoubleSpinBox *m_scaleSpin = nullptr;
    QDoubleSpinBox *m_rotation2DSpin = nullptr;
    QDoubleSpinBox *m_opacitySpin = nullptr;

    EffectKeyframeToggle *m_posZToggle = nullptr;
    EffectKeyframeToggle *m_rotXToggle = nullptr;
    EffectKeyframeToggle *m_rotYToggle = nullptr;
    EffectKeyframeToggle *m_rotZToggle = nullptr;

    QDoubleSpinBox *m_posZSpin = nullptr;
    QDoubleSpinBox *m_rotXSpin = nullptr;
    QDoubleSpinBox *m_rotYSpin = nullptr;
    QDoubleSpinBox *m_rotZSpin = nullptr;

    QLabel *m_titleLabel = nullptr;
    QVector<MotionRowWidgets> m_motionRows;
};

} // namespace effectctrl

Q_DECLARE_METATYPE(effectctrl::MotionState)
