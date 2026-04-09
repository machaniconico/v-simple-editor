#include "OverlayDialogs.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QFileDialog>

// --- TextOverlayDialog ---

TextOverlayDialog::TextOverlayDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Add Text / Telop");
    setMinimumWidth(450);
    setupUI();
}

void TextOverlayDialog::setupUI()
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    m_textEdit = new QLineEdit(this);
    m_textEdit->setPlaceholderText("Enter text...");
    form->addRow("Text:", m_textEdit);

    m_fontCombo = new QFontComboBox(this);
    form->addRow("Font:", m_fontCombo);

    m_fontSizeSpin = new QSpinBox(this);
    m_fontSizeSpin->setRange(8, 200);
    m_fontSizeSpin->setValue(32);
    form->addRow("Size:", m_fontSizeSpin);

    m_colorBtn = new QPushButton("White", this);
    m_colorBtn->setStyleSheet("background-color: white; color: black;");
    connect(m_colorBtn, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_textColor, this, "Text Color");
        if (c.isValid()) { m_textColor = c; m_colorBtn->setStyleSheet(QString("background-color: %1;").arg(c.name())); }
    });
    form->addRow("Color:", m_colorBtn);

    m_bgColorBtn = new QPushButton("Semi-Black", this);
    m_bgColorBtn->setStyleSheet("background-color: rgba(0,0,0,160);");
    connect(m_bgColorBtn, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_bgColor, this, "Background Color", QColorDialog::ShowAlphaChannel);
        if (c.isValid()) { m_bgColor = c; m_bgColorBtn->setStyleSheet(QString("background-color: %1;").arg(c.name())); }
    });
    form->addRow("Background:", m_bgColorBtn);

    m_positionPreset = new QComboBox(this);
    m_positionPreset->addItems({"Bottom Center (Subtitle)", "Top Center", "Center", "Bottom Left", "Bottom Right", "Custom"});
    connect(m_positionPreset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        double positions[][2] = {{0.5, 0.85}, {0.5, 0.1}, {0.5, 0.5}, {0.15, 0.85}, {0.85, 0.85}};
        if (idx < 5) { m_xSpin->setValue(positions[idx][0]); m_ySpin->setValue(positions[idx][1]); }
        bool custom = (idx == 5);
        m_xSpin->setEnabled(custom); m_ySpin->setEnabled(custom);
    });
    form->addRow("Position:", m_positionPreset);

    m_xSpin = new QDoubleSpinBox(this);
    m_xSpin->setRange(0.0, 1.0); m_xSpin->setValue(0.5); m_xSpin->setSingleStep(0.05);
    m_xSpin->setEnabled(false);
    m_ySpin = new QDoubleSpinBox(this);
    m_ySpin->setRange(0.0, 1.0); m_ySpin->setValue(0.85); m_ySpin->setSingleStep(0.05);
    m_ySpin->setEnabled(false);
    auto *posLayout = new QHBoxLayout();
    posLayout->addWidget(new QLabel("X:")); posLayout->addWidget(m_xSpin);
    posLayout->addWidget(new QLabel("Y:")); posLayout->addWidget(m_ySpin);
    form->addRow("", posLayout);

    m_startSpin = new QDoubleSpinBox(this);
    m_startSpin->setRange(0.0, 9999.0); m_startSpin->setSuffix(" s");
    m_endSpin = new QDoubleSpinBox(this);
    m_endSpin->setRange(0.0, 9999.0); m_endSpin->setSuffix(" s (0=end)");
    form->addRow("Start:", m_startSpin);
    form->addRow("End:", m_endSpin);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_result.text = m_textEdit->text();
        m_result.font = QFont(m_fontCombo->currentFont().family(), m_fontSizeSpin->value(), QFont::Bold);
        m_result.color = m_textColor;
        m_result.backgroundColor = m_bgColor;
        m_result.x = m_xSpin->value();
        m_result.y = m_ySpin->value();
        m_result.startTime = m_startSpin->value();
        m_result.endTime = m_endSpin->value();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// --- TransitionDialog ---

TransitionDialog::TransitionDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Add Transition");
    setupUI();
}

void TransitionDialog::setupUI()
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    m_typeCombo = new QComboBox(this);
    m_typeCombo->addItem("Fade In",         static_cast<int>(TransitionType::FadeIn));
    m_typeCombo->addItem("Fade Out",        static_cast<int>(TransitionType::FadeOut));
    m_typeCombo->addItem("Cross Dissolve",  static_cast<int>(TransitionType::CrossDissolve));
    m_typeCombo->addItem("Wipe Left",       static_cast<int>(TransitionType::WipeLeft));
    m_typeCombo->addItem("Wipe Right",      static_cast<int>(TransitionType::WipeRight));
    m_typeCombo->addItem("Wipe Up",         static_cast<int>(TransitionType::WipeUp));
    m_typeCombo->addItem("Wipe Down",       static_cast<int>(TransitionType::WipeDown));
    m_typeCombo->addItem("Slide Left",      static_cast<int>(TransitionType::SlideLeft));
    m_typeCombo->addItem("Slide Right",     static_cast<int>(TransitionType::SlideRight));
    form->addRow("Type:", m_typeCombo);

    m_durationSpin = new QDoubleSpinBox(this);
    m_durationSpin->setRange(0.1, 5.0);
    m_durationSpin->setValue(0.5);
    m_durationSpin->setSingleStep(0.1);
    m_durationSpin->setSuffix(" s");
    form->addRow("Duration:", m_durationSpin);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_result.type = static_cast<TransitionType>(m_typeCombo->currentData().toInt());
        m_result.duration = m_durationSpin->value();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// --- ImageOverlayDialog ---

ImageOverlayDialog::ImageOverlayDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Add Image / Still");
    setMinimumWidth(400);
    setupUI();
}

void ImageOverlayDialog::setupUI()
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    auto *pathLayout = new QHBoxLayout();
    m_pathEdit = new QLineEdit(this);
    auto *browseBtn = new QPushButton("Browse...", this);
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "Select Image", QString(),
            "Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;All Files (*)");
        if (!path.isEmpty()) m_pathEdit->setText(path);
    });
    pathLayout->addWidget(m_pathEdit);
    pathLayout->addWidget(browseBtn);
    form->addRow("Image:", pathLayout);

    m_xSpin = new QDoubleSpinBox(this); m_xSpin->setRange(0, 1); m_xSpin->setValue(0.1); m_xSpin->setSingleStep(0.05);
    m_ySpin = new QDoubleSpinBox(this); m_ySpin->setRange(0, 1); m_ySpin->setValue(0.1); m_ySpin->setSingleStep(0.05);
    m_wSpin = new QDoubleSpinBox(this); m_wSpin->setRange(0.01, 1); m_wSpin->setValue(0.3); m_wSpin->setSingleStep(0.05);
    m_hSpin = new QDoubleSpinBox(this); m_hSpin->setRange(0.01, 1); m_hSpin->setValue(0.3); m_hSpin->setSingleStep(0.05);
    form->addRow("X:", m_xSpin); form->addRow("Y:", m_ySpin);
    form->addRow("Width:", m_wSpin); form->addRow("Height:", m_hSpin);

    m_opacitySpin = new QDoubleSpinBox(this); m_opacitySpin->setRange(0, 1); m_opacitySpin->setValue(1.0); m_opacitySpin->setSingleStep(0.1);
    form->addRow("Opacity:", m_opacitySpin);

    m_aspectCheck = new QCheckBox("Keep aspect ratio", this);
    m_aspectCheck->setChecked(true);
    form->addRow("", m_aspectCheck);

    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_result.filePath = m_pathEdit->text();
        m_result.rect = QRectF(m_xSpin->value(), m_ySpin->value(), m_wSpin->value(), m_hSpin->value());
        m_result.opacity = m_opacitySpin->value();
        m_result.keepAspectRatio = m_aspectCheck->isChecked();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// --- PipDialog ---

PipDialog::PipDialog(int maxClipIndex, QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Picture in Picture");
    setupUI(maxClipIndex);
}

void PipDialog::setupUI(int maxClipIndex)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    m_clipSpin = new QSpinBox(this);
    m_clipSpin->setRange(0, qMax(0, maxClipIndex));
    form->addRow("Source Clip #:", m_clipSpin);

    m_positionCombo = new QComboBox(this);
    m_positionCombo->addItem("Top Left",     static_cast<int>(PipConfig::TopLeft));
    m_positionCombo->addItem("Top Right",    static_cast<int>(PipConfig::TopRight));
    m_positionCombo->addItem("Bottom Left",  static_cast<int>(PipConfig::BottomLeft));
    m_positionCombo->addItem("Bottom Right", static_cast<int>(PipConfig::BottomRight));
    m_positionCombo->setCurrentIndex(1); // Top Right default
    form->addRow("Position:", m_positionCombo);

    m_sizeSpin = new QDoubleSpinBox(this);
    m_sizeSpin->setRange(0.1, 0.8); m_sizeSpin->setValue(0.3); m_sizeSpin->setSingleStep(0.05);
    form->addRow("Size:", m_sizeSpin);

    m_opacitySpin = new QDoubleSpinBox(this);
    m_opacitySpin->setRange(0, 1); m_opacitySpin->setValue(1.0); m_opacitySpin->setSingleStep(0.1);
    form->addRow("Opacity:", m_opacitySpin);

    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_result.sourceClipIndex = m_clipSpin->value();
        auto pos = static_cast<PipConfig::Position>(m_positionCombo->currentData().toInt());
        m_result.rect = PipConfig::presetRect(pos);
        double sz = m_sizeSpin->value();
        m_result.rect.setWidth(sz); m_result.rect.setHeight(sz);
        m_result.opacity = m_opacitySpin->value();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
