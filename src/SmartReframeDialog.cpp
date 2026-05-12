#include "SmartReframeDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLabel>
#include <QGroupBox>
#include <QComboBox>
#include <QSpinBox>
#include <QSlider>
#include <QCheckBox>

SmartReframeDialog::SmartReframeDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Smart Reframe"));
    setModal(true);
    setMinimumWidth(420);

    setupUI();
}

void SmartReframeDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);

    // ---- Aspect ratio preset ------------------------------------------------
    auto *aspectGroup = new QGroupBox(tr("Aspect Ratio"), this);
    auto *aspectLayout = new QVBoxLayout(aspectGroup);

    m_aspectPresetCombo = new QComboBox(aspectGroup);
    m_aspectPresetCombo->addItem(tr("9:16 (TikTok/Reels/Shorts)"), 0);
    m_aspectPresetCombo->addItem(tr("1:1 (Instagram square)"), 1);
    m_aspectPresetCombo->addItem(tr("4:5 (Instagram portrait)"), 2);
    m_aspectPresetCombo->addItem(tr("16:9 (landscape)"), 3);
    m_aspectPresetCombo->addItem(tr("\u30ab\u30b9\u30bf\u30e0..."), 4); // カスタム...
    m_aspectPresetCombo->setCurrentIndex(0);
    aspectLayout->addWidget(m_aspectPresetCombo);

    // Custom W/H spin boxes (disabled by default)
    auto *customRow = new QHBoxLayout();
    m_customWSpin = new QSpinBox(aspectGroup);
    m_customWSpin->setRange(1, 9999);
    m_customWSpin->setValue(9);
    m_customWSpin->setEnabled(false);
    m_customHSpin = new QSpinBox(aspectGroup);
    m_customHSpin->setRange(1, 9999);
    m_customHSpin->setValue(16);
    m_customHSpin->setEnabled(false);
    customRow->addWidget(new QLabel(tr("W:"), aspectGroup));
    customRow->addWidget(m_customWSpin);
    customRow->addWidget(new QLabel(tr("H:"), aspectGroup));
    customRow->addWidget(m_customHSpin);
    customRow->addStretch(1);
    aspectLayout->addLayout(customRow);

    connect(m_aspectPresetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SmartReframeDialog::onAspectPresetChanged);

    mainLayout->addWidget(aspectGroup);

    // ---- Output resolution --------------------------------------------------
    auto *resGroup = new QGroupBox(tr("Output Resolution"), this);
    auto *resLayout = new QVBoxLayout(resGroup);

    m_outputResCombo = new QComboBox(resGroup);
    m_outputResCombo->addItem(tr("1080x1920"), 0);
    m_outputResCombo->addItem(tr("1080x1350"), 1);
    m_outputResCombo->addItem(tr("1080x1080"), 2);
    m_outputResCombo->addItem(tr("720x1280"), 3);
    m_outputResCombo->addItem(tr("\u30bd\u30fc\u30b9\u306b\u5408\u308f\u305b\u308b"), 4); // ソースに合わせる
    m_outputResCombo->setCurrentIndex(0);
    m_outputResLabel = new QLabel(resGroup);
    resLayout->addWidget(m_outputResCombo);
    resLayout->addWidget(m_outputResLabel);

    connect(m_outputResCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SmartReframeDialog::onOutputResolutionChanged);

    mainLayout->addWidget(resGroup);

    // ---- Smoothness slider --------------------------------------------------
    auto *smoothGroup = new QGroupBox(tr("Smoothness"), this);
    auto *smoothLayout = new QVBoxLayout(smoothGroup);

    m_smoothnessSlider = new QSlider(Qt::Horizontal, smoothGroup);
    m_smoothnessSlider->setRange(0, 100);
    m_smoothnessSlider->setValue(70);
    m_smoothnessLabel = new QLabel(smoothGroup);
    smoothLayout->addWidget(m_smoothnessSlider);
    smoothLayout->addWidget(m_smoothnessLabel);

    mainLayout->addWidget(smoothGroup);

    // ---- Motion weight slider -----------------------------------------------
    auto *motionGroup = new QGroupBox(tr("Motion Weight"), this);
    auto *motionLayout = new QVBoxLayout(motionGroup);

    m_motionWeightSlider = new QSlider(Qt::Horizontal, motionGroup);
    m_motionWeightSlider->setRange(0, 100);
    m_motionWeightSlider->setValue(50);
    m_motionWeightLabel = new QLabel(motionGroup);
    motionLayout->addWidget(m_motionWeightSlider);
    motionLayout->addWidget(m_motionWeightLabel);

    mainLayout->addWidget(motionGroup);

    // ---- Padding percent ----------------------------------------------------
    auto *paddingGroup = new QGroupBox(tr("Padding"), this);
    auto *paddingLayout = new QHBoxLayout(paddingGroup);

    m_paddingSpin = new QSpinBox(paddingGroup);
    m_paddingSpin->setRange(0, 30);
    m_paddingSpin->setValue(8);
    m_paddingSpin->setSuffix(" %");
    paddingLayout->addWidget(new QLabel(tr("Extra margin around subject:"), paddingGroup));
    paddingLayout->addWidget(m_paddingSpin);
    paddingLayout->addStretch(1);

    mainLayout->addWidget(paddingGroup);

    // ---- Use motion checkbox ------------------------------------------------
    m_useMotionCheck = new QCheckBox(tr("\u30e2\u30fc\u30b7\u30e7\u30f3\u3092\u8003\u616e (\u52d5\u304f\u88ab\u5199\u4f53\u3092\u512a\u5148\u8ffd\u5f93)"), this);
    m_useMotionCheck->setChecked(true);
    mainLayout->addWidget(m_useMotionCheck);

    // ---- OK / Cancel buttons ------------------------------------------------
    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(m_buttonBox);

    // Initialize labels
    onAspectPresetChanged(0);
    onOutputResolutionChanged(0);
}

SmartReframeParams SmartReframeDialog::params() const
{
    SmartReframeParams p;

    const int presetIdx = m_aspectPresetCombo->currentIndex();
    if (presetIdx == 4) {
        // カスタム...
        p.aspectW = static_cast<double>(m_customWSpin->value());
        p.aspectH = static_cast<double>(m_customHSpin->value());
    } else {
        switch (presetIdx) {
        case 0:  // 9:16
            p.aspectW = 9.0;  p.aspectH = 16.0; break;
        case 1:  // 1:1
            p.aspectW = 1.0;  p.aspectH = 1.0;  break;
        case 2:  // 4:5
            p.aspectW = 4.0;  p.aspectH = 5.0;  break;
        case 3:  // 16:9
            p.aspectW = 16.0; p.aspectH = 9.0;  break;
        default:
            p.aspectW = 9.0;  p.aspectH = 16.0; break;
        }
    }

    const int resIdx = m_outputResCombo->currentIndex();
    switch (resIdx) {
    case 0:  p.outputSize = QSize(1080, 1920); break;
    case 1:  p.outputSize = QSize(1080, 1350); break;
    case 2:  p.outputSize = QSize(1080, 1080); break;
    case 3:  p.outputSize = QSize(720, 1280);  break;
    case 4:  p.outputSize = QSize(0, 0);       break; // source
    default: p.outputSize = QSize(1080, 1920); break;
    }

    p.smoothness    = static_cast<double>(m_smoothnessSlider->value()) / 100.0;
    p.motionWeight  = static_cast<double>(m_motionWeightSlider->value()) / 100.0;
    p.paddingPercent = static_cast<double>(m_paddingSpin->value());
    p.useMotion     = m_useMotionCheck->isChecked();

    return p;
}

void SmartReframeDialog::onAspectPresetChanged(int index)
{
    const bool custom = (index == 4);
    m_customWSpin->setEnabled(custom);
    m_customHSpin->setEnabled(custom);

    // Update resolution combo to match the selected aspect ratio
    switch (index) {
    case 0: // 9:16
        m_outputResCombo->setCurrentIndex(0); // 1080x1920
        break;
    case 1: // 1:1
        m_outputResCombo->setCurrentIndex(2); // 1080x1080
        break;
    case 2: // 4:5
        m_outputResCombo->setCurrentIndex(1); // 1080x1350
        break;
    case 3: // 16:9
        m_outputResCombo->setCurrentIndex(3); // 720x1280 (closest, or user picks source)
        break;
    default:
        break;
    }
}

void SmartReframeDialog::onOutputResolutionChanged(int index)
{
    QSize size;
    switch (index) {
    case 0:  size = QSize(1080, 1920); break;
    case 1:  size = QSize(1080, 1350); break;
    case 2:  size = QSize(1080, 1080); break;
    case 3:  size = QSize(720, 1280);  break;
    case 4:  size = QSize(0, 0);       break;
    default: size = QSize(1080, 1920); break;
    }

    if (size.isValid()) {
        m_outputResLabel->setText(tr("Output: %1x%2").arg(size.width()).arg(size.height()));
    } else {
        m_outputResLabel->setText(tr("Output: source resolution"));
    }
}
