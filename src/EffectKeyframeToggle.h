#pragma once

#include <QToolButton>

namespace effectctrl {

class EffectKeyframeToggle : public QToolButton
{
    Q_OBJECT

public:
    explicit EffectKeyframeToggle(QWidget *parent = nullptr);

    void setHasTrack(bool has);
    bool hasTrack() const;

signals:
    void toggled(bool now);

private:
    bool m_hasTrack = false;
};

} // namespace effectctrl
