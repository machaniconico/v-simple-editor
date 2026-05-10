#include "EffectKeyframeToggle.h"
#include <QIcon>

namespace effectctrl {

EffectKeyframeToggle::EffectKeyframeToggle(QWidget *parent)
    : QToolButton(parent)
{
    setFixedSize(20, 20);
    setCheckable(true);
    setChecked(false);
    setToolTip("Toggle keyframe animation");
    // No dedicated clock icon in resources; use effects.svg as closest visual match
    setIcon(QIcon(QStringLiteral(":/icons/effects.svg")));
    setIconSize(QSize(14, 14));

    connect(this, &QToolButton::toggled, this, [this](bool checked) {
        m_hasTrack = checked;
        emit toggled(checked);
    });
}

void EffectKeyframeToggle::setHasTrack(bool has)
{
    m_hasTrack = has;
    blockSignals(true);
    setChecked(has);
    blockSignals(false);
}

bool EffectKeyframeToggle::hasTrack() const
{
    return m_hasTrack;
}

} // namespace effectctrl
