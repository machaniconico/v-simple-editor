#pragma once

#include <QWidget>
#include <QToolButton>
#include <QPushButton>
#include <QMenu>
#include "VideoEffect.h"

namespace effectctrl {

class EffectControlsRowToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit EffectControlsRowToolbar(int effectIndex, QWidget *parent = nullptr);

    int effectIndex() const { return m_effectIndex; }
    void setEffectIndex(int idx) { m_effectIndex = idx; }
    void setBypassed(bool bypassed) { if (m_bypassBtn) m_bypassBtn->setChecked(bypassed); }

signals:
    void bypassToggled(int idx, bool enabled);
    void resetRequested(int idx);
    void duplicateRequested(int idx);
    void removeRequested(int idx);
    void moveUpRequested(int idx);
    void moveDownRequested(int idx);

private:
    int m_effectIndex;
    QToolButton *m_bypassBtn = nullptr;
    QToolButton *m_resetBtn = nullptr;
    QToolButton *m_duplicateBtn = nullptr;
    QToolButton *m_removeBtn = nullptr;
    QToolButton *m_moveUpBtn = nullptr;
    QToolButton *m_moveDownBtn = nullptr;
};

class EffectControlsPanelToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit EffectControlsPanelToolbar(QWidget *parent = nullptr);

signals:
    void addEffectRequested(VideoEffectType type);
    void collapseAllRequested();
    void expandAllRequested();

private:
    QPushButton *m_addEffectBtn = nullptr;
    QMenu *m_addEffectMenu = nullptr;
};

} // namespace effectctrl
