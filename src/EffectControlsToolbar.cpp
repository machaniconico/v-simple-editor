#include "EffectControlsToolbar.h"
#include <QHBoxLayout>
#include <QIcon>
#include <QStyle>

namespace effectctrl {

// ── EffectControlsRowToolbar ──────────────────────────────────────────

EffectControlsRowToolbar::EffectControlsRowToolbar(int effectIndex, QWidget *parent)
    : QWidget(parent), m_effectIndex(effectIndex)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    const int btnSize = 20;

    // Bypass toggle (eye icon — use SP_TitleBarNormalButton as closest standard, fallback to text)
    m_bypassBtn = new QToolButton(this);
    m_bypassBtn->setFixedSize(btnSize, btnSize);
    m_bypassBtn->setCheckable(true);
    m_bypassBtn->setChecked(true);
    m_bypassBtn->setToolTip("Bypass effect");
    QIcon eyeIcon = QIcon(":/icons/effects.svg");
    if (!eyeIcon.isNull()) m_bypassBtn->setIcon(eyeIcon);
    else m_bypassBtn->setText("fx");
    connect(m_bypassBtn, &QToolButton::toggled, this, [this](bool checked) {
        emit bypassToggled(m_effectIndex, checked);
    });
    layout->addWidget(m_bypassBtn);

    // Reset all
    m_resetBtn = new QToolButton(this);
    m_resetBtn->setFixedSize(btnSize, btnSize);
    m_resetBtn->setToolTip("Reset all parameters");
    m_resetBtn->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    connect(m_resetBtn, &QToolButton::clicked, this, [this]() {
        emit resetRequested(m_effectIndex);
    });
    layout->addWidget(m_resetBtn);

    // Duplicate
    m_duplicateBtn = new QToolButton(this);
    m_duplicateBtn->setFixedSize(btnSize, btnSize);
    m_duplicateBtn->setToolTip("Duplicate effect");
    m_duplicateBtn->setIcon(QIcon(":/icons/copy.svg"));
    connect(m_duplicateBtn, &QToolButton::clicked, this, [this]() {
        emit duplicateRequested(m_effectIndex);
    });
    layout->addWidget(m_duplicateBtn);

    // Remove
    m_removeBtn = new QToolButton(this);
    m_removeBtn->setFixedSize(btnSize, btnSize);
    m_removeBtn->setToolTip("Remove effect");
    m_removeBtn->setIcon(QIcon(":/icons/delete.svg"));
    connect(m_removeBtn, &QToolButton::clicked, this, [this]() {
        emit removeRequested(m_effectIndex);
    });
    layout->addWidget(m_removeBtn);

    // Move up
    m_moveUpBtn = new QToolButton(this);
    m_moveUpBtn->setFixedSize(btnSize, btnSize);
    m_moveUpBtn->setToolTip("Move effect up");
    m_moveUpBtn->setIcon(QIcon(":/icons/spin-up.svg"));
    connect(m_moveUpBtn, &QToolButton::clicked, this, [this]() {
        emit moveUpRequested(m_effectIndex);
    });
    layout->addWidget(m_moveUpBtn);

    // Move down
    m_moveDownBtn = new QToolButton(this);
    m_moveDownBtn->setFixedSize(btnSize, btnSize);
    m_moveDownBtn->setToolTip("Move effect down");
    m_moveDownBtn->setIcon(QIcon(":/icons/spin-down.svg"));
    connect(m_moveDownBtn, &QToolButton::clicked, this, [this]() {
        emit moveDownRequested(m_effectIndex);
    });
    layout->addWidget(m_moveDownBtn);

    layout->addStretch();
}

// ── EffectControlsPanelToolbar ────────────────────────────────────────

EffectControlsPanelToolbar::EffectControlsPanelToolbar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(4);

    // Add Effect dropdown
    m_addEffectBtn = new QPushButton("Add Effect", this);
    m_addEffectMenu = new QMenu(m_addEffectBtn);

    const auto allTypes = VideoEffect::allTypes();
    for (VideoEffectType type : allTypes) {
        if (type == VideoEffectType::None) continue;
        QAction *action = m_addEffectMenu->addAction(VideoEffect::typeName(type));
        connect(action, &QAction::triggered, this, [this, type]() {
            emit addEffectRequested(type);
        });
    }
    m_addEffectBtn->setMenu(m_addEffectMenu);
    layout->addWidget(m_addEffectBtn);

    layout->addStretch();
}

} // namespace effectctrl
