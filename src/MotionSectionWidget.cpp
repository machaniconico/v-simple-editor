#include "MotionSectionWidget.h"
#include "EffectKeyframeNavBar.h"
#include "EffectKeyframeToggle.h"
#include "Keyframe.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFrame>

namespace effectctrl {

MotionSectionWidget::MotionSectionWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(6);

    auto *toggleRow = new QHBoxLayout();
    toggleRow->setContentsMargins(0, 0, 0, 0);
    toggleRow->setSpacing(6);

    m_titleLabel = new QLabel(tr("Motion"), this);
    m_titleLabel->setStyleSheet("font-weight: 600;");
    toggleRow->addWidget(m_titleLabel);
    toggleRow->addStretch();

    m_toggle3D = new QToolButton(this);
    m_toggle3D->setCheckable(true);
    m_toggle3D->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_toggle3D->setIcon(QIcon(QStringLiteral(":/icons/effects.svg")));
    m_toggle3D->setText(tr("3D Layer"));
    toggleRow->addWidget(m_toggle3D);

    rootLayout->addLayout(toggleRow);

    // 2D motion rows with keyframe toggles
    auto *layout2D = new QVBoxLayout();
    layout2D->setContentsMargins(0, 0, 0, 0);
    layout2D->setSpacing(4);

    m_posXSpin = createSpinBox(-10000.0, 10000.0, 1.0, 3, 0.0);
    m_posYSpin = createSpinBox(-10000.0, 10000.0, 1.0, 3, 0.0);
    m_scaleSpin = createSpinBox(0.0, 10.0, 0.01, 4, 1.0);
    m_rotation2DSpin = createSpinBox(-360.0, 360.0, 1.0, 3, 0.0);
    m_opacitySpin = createSpinBox(0.0, 1.0, 0.01, 4, 1.0);

    m_posXToggle = new EffectKeyframeToggle(this);
    m_posYToggle = new EffectKeyframeToggle(this);
    m_scaleToggle = new EffectKeyframeToggle(this);
    m_rotation2DToggle = new EffectKeyframeToggle(this);
    m_opacityToggle = new EffectKeyframeToggle(this);

    auto addMotionRow = [this, layout2D](QLabel *title, const QString &propPath,
                                         EffectKeyframeToggle *toggle, QDoubleSpinBox *spin) {
        auto *container = new QWidget(this);
        auto *containerLayout = new QVBoxLayout(container);
        containerLayout->setContentsMargins(0, 0, 0, 0);
        containerLayout->setSpacing(2);

        auto *row = new QHBoxLayout();
        row->setContentsMargins(0, 1, 0, 1);
        row->setSpacing(4);
        row->addWidget(toggle);
        row->addWidget(title);
        row->addWidget(spin, 1);
        containerLayout->addLayout(row);
        layout2D->addWidget(container);

        MotionRowWidgets motionRow;
        motionRow.propPath = propPath;
        motionRow.container = container;
        motionRow.containerLayout = containerLayout;
        motionRow.toggle = toggle;
        motionRow.spin = spin;
        m_motionRows.append(motionRow);
    };

    addMotionRow(new QLabel(tr("Position-x"), this), QStringLiteral("motion.position.x"), m_posXToggle, m_posXSpin);
    addMotionRow(new QLabel(tr("Position-y"), this), QStringLiteral("motion.position.y"), m_posYToggle, m_posYSpin);
    addMotionRow(new QLabel(tr("Scale-uniform"), this), QStringLiteral("motion.scale"), m_scaleToggle, m_scaleSpin);
    addMotionRow(new QLabel(tr("Rotation-2D"), this), QStringLiteral("motion.rotation"), m_rotation2DToggle, m_rotation2DSpin);
    addMotionRow(new QLabel(tr("Opacity"), this), QStringLiteral("motion.opacity"), m_opacityToggle, m_opacitySpin);
    rootLayout->addLayout(layout2D);

    m_group3D = new QFrame(this);
    m_group3D->setFrameShape(QFrame::StyledPanel);
    auto *layout3D = new QVBoxLayout(m_group3D);
    layout3D->setContentsMargins(8, 8, 8, 8);
    layout3D->setSpacing(4);

    m_posZSpin = createSpinBox(-10000.0, 10000.0, 1.0, 3, 0.0);
    m_rotXSpin = createSpinBox(-360.0, 360.0, 1.0, 3, 0.0);
    m_rotYSpin = createSpinBox(-360.0, 360.0, 1.0, 3, 0.0);
    m_rotZSpin = createSpinBox(-360.0, 360.0, 1.0, 3, 0.0);

    m_posZToggle = new EffectKeyframeToggle(this);
    m_rotXToggle = new EffectKeyframeToggle(this);
    m_rotYToggle = new EffectKeyframeToggle(this);
    m_rotZToggle = new EffectKeyframeToggle(this);

    auto add3DRow = [this, layout3D](QLabel *title, const QString &propPath,
                                     EffectKeyframeToggle *toggle, QDoubleSpinBox *spin) {
        auto *container = new QWidget(this);
        auto *containerLayout = new QVBoxLayout(container);
        containerLayout->setContentsMargins(0, 0, 0, 0);
        containerLayout->setSpacing(2);

        auto *row = new QHBoxLayout();
        row->setContentsMargins(0, 1, 0, 1);
        row->setSpacing(4);
        row->addWidget(toggle);
        row->addWidget(title);
        row->addWidget(spin, 1);
        containerLayout->addLayout(row);
        layout3D->addWidget(container);

        MotionRowWidgets motionRow;
        motionRow.propPath = propPath;
        motionRow.container = container;
        motionRow.containerLayout = containerLayout;
        motionRow.toggle = toggle;
        motionRow.spin = spin;
        m_motionRows.append(motionRow);
    };

    add3DRow(new QLabel(tr("Position-z"), this), QStringLiteral("motion.position.z"), m_posZToggle, m_posZSpin);
    add3DRow(new QLabel(tr("Rotation-X"), this), QStringLiteral("motion.rotation.x"), m_rotXToggle, m_rotXSpin);
    add3DRow(new QLabel(tr("Rotation-Y"), this), QStringLiteral("motion.rotation.y"), m_rotYToggle, m_rotYSpin);
    add3DRow(new QLabel(tr("Rotation-Z"), this), QStringLiteral("motion.rotation.z"), m_rotZToggle, m_rotZSpin);
    rootLayout->addWidget(m_group3D);

    bindMotionRow(m_posXToggle, m_posXSpin, QStringLiteral("motion.position.x"));
    bindMotionRow(m_posYToggle, m_posYSpin, QStringLiteral("motion.position.y"));
    bindMotionRow(m_scaleToggle, m_scaleSpin, QStringLiteral("motion.scale"));
    bindMotionRow(m_rotation2DToggle, m_rotation2DSpin, QStringLiteral("motion.rotation"));
    bindMotionRow(m_opacityToggle, m_opacitySpin, QStringLiteral("motion.opacity"));
    bindMotionRow(m_posZToggle, m_posZSpin, QStringLiteral("motion.position.z"));
    bindMotionRow(m_rotXToggle, m_rotXSpin, QStringLiteral("motion.rotation.x"));
    bindMotionRow(m_rotYToggle, m_rotYSpin, QStringLiteral("motion.rotation.y"));
    bindMotionRow(m_rotZToggle, m_rotZSpin, QStringLiteral("motion.rotation.z"));

    connect(m_toggle3D, &QToolButton::toggled, this, [this](bool) {
        sync3DVisibility();
        emitMotionChanged();
    });

    sync3DVisibility();
}

void MotionSectionWidget::setMotion(const MotionState &motion)
{
    const QSignalBlocker blockToggle(m_toggle3D);
    const QSignalBlocker blockPosX(m_posXSpin);
    const QSignalBlocker blockPosY(m_posYSpin);
    const QSignalBlocker blockScale(m_scaleSpin);
    const QSignalBlocker blockRot2D(m_rotation2DSpin);
    const QSignalBlocker blockOpacity(m_opacitySpin);
    const QSignalBlocker blockPosZ(m_posZSpin);
    const QSignalBlocker blockRotX(m_rotXSpin);
    const QSignalBlocker blockRotY(m_rotYSpin);
    const QSignalBlocker blockRotZ(m_rotZSpin);

    m_toggle3D->setChecked(motion.is3DLayer);
    m_posXSpin->setValue(motion.dx);
    m_posYSpin->setValue(motion.dy);
    m_scaleSpin->setValue(motion.scale);
    m_rotation2DSpin->setValue(motion.rotation2DDeg);
    m_opacitySpin->setValue(motion.opacity);
    m_posZSpin->setValue(motion.posZ);
    m_rotXSpin->setValue(motion.rotX);
    m_rotYSpin->setValue(motion.rotY);
    m_rotZSpin->setValue(motion.rotZ);
    sync3DVisibility();
}

MotionState MotionSectionWidget::currentMotion() const
{
    MotionState motion;
    motion.scale = m_scaleSpin->value();
    motion.dx = m_posXSpin->value();
    motion.dy = m_posYSpin->value();
    motion.rotation2DDeg = m_rotation2DSpin->value();
    motion.opacity = m_opacitySpin->value();
    motion.is3DLayer = m_toggle3D->isChecked();
    if (motion.is3DLayer) {
        motion.posZ = m_posZSpin->value();
        motion.rotX = m_rotXSpin->value();
        motion.rotY = m_rotYSpin->value();
        motion.rotZ = m_rotZSpin->value();
    }
    return motion;
}

QDoubleSpinBox *MotionSectionWidget::createSpinBox(double min, double max, double step,
                                                   int decimals, double value)
{
    auto *spin = new QDoubleSpinBox(this);
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setValue(value);
    spin->setKeyboardTracking(false);
    return spin;
}

void MotionSectionWidget::emitMotionChanged()
{
    emit motionChanged(currentMotion());
}

void MotionSectionWidget::sync3DVisibility()
{
    if (m_group3D)
        m_group3D->setVisible(m_toggle3D && m_toggle3D->isChecked());
}

void MotionSectionWidget::bindMotionRow(EffectKeyframeToggle *toggle, QDoubleSpinBox *spin, const QString &propPath)
{
    connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double) { emitMotionChanged(); });
    connect(toggle, &EffectKeyframeToggle::toggled, this, [this, propPath](bool now) {
        emit keyframeToggled(propPath, now);
    });
}

double MotionSectionWidget::getMotionValue(const QString &propPath) const
{
    if (const MotionRowWidgets *row = findMotionRow(propPath)) {
        return row->spin ? row->spin->value() : 0.0;
    }
    return 0.0;
}

void MotionSectionWidget::setPropHasTrack(const QString &propPath, bool has)
{
    if (MotionRowWidgets *row = findMotionRow(propPath)) {
        row->toggle->setHasTrack(has);
    }
}

bool MotionSectionWidget::propHasTrack(const QString &propPath) const
{
    if (const MotionRowWidgets *row = findMotionRow(propPath)) {
        return row->toggle && row->toggle->hasTrack();
    }
    return false;
}

void MotionSectionWidget::setPropKeyframeTrack(const QString &propPath, KeyframeTrack *track,
                                               double clipDurationSeconds, double playheadSeconds)
{
    MotionRowWidgets *row = findMotionRow(propPath);
    if (!row) {
        return;
    }

    const bool shouldShow = row->toggle && row->toggle->hasTrack() && track;
    if (!shouldShow) {
        if (row->navBar) {
            row->containerLayout->removeWidget(row->navBar);
            delete row->navBar;
            row->navBar = nullptr;
        }
        return;
    }

    if (!row->navBar) {
        row->navBar = new EffectKeyframeNavBar(row->container);
        row->navBar->setFixedHeight(24);
        row->containerLayout->addWidget(row->navBar);
        connect(row->navBar, &EffectKeyframeNavBar::trackChanged, this, [this, propPath]() {
            emit keyframeTrackChanged(propPath);
        });
    }

    row->navBar->setTrack(track, clipDurationSeconds, playheadSeconds);
}

void MotionSectionWidget::setPlayhead(double seconds)
{
    for (auto &row : m_motionRows) {
        if (row.navBar) {
            row.navBar->setPlayhead(seconds);
        }
    }
}

MotionSectionWidget::MotionRowWidgets *MotionSectionWidget::findMotionRow(const QString &propPath)
{
    for (auto &row : m_motionRows) {
        if (row.propPath == propPath) {
            return &row;
        }
    }
    return nullptr;
}

const MotionSectionWidget::MotionRowWidgets *MotionSectionWidget::findMotionRow(const QString &propPath) const
{
    for (const auto &row : m_motionRows) {
        if (row.propPath == propPath) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace effectctrl
