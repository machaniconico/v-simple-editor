#include "OverlayDialogs.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>

// --- TransitionPresetStore ---

QList<TransitionPreset> TransitionPresetStore::loadAll()
{
    QSettings prefs("VSimpleEditor", "Preferences");
    QList<TransitionPreset> out;
    const int size = prefs.beginReadArray("transitionPresets");
    for (int i = 0; i < size; ++i) {
        prefs.setArrayIndex(i);
        TransitionPreset p;
        p.name = prefs.value("name").toString();
        if (p.name.isEmpty()) continue;
        p.transition.type = static_cast<TransitionType>(
            prefs.value("type", static_cast<int>(TransitionType::CrossDissolve)).toInt());
        p.transition.duration = prefs.value("duration", 1.0).toDouble();
        p.transition.alignment = static_cast<TransitionAlignment>(
            prefs.value("alignment", static_cast<int>(TransitionAlignment::Center)).toInt());
        p.transition.easing = static_cast<TransitionEasing>(
            prefs.value("easing", static_cast<int>(TransitionEasing::Linear)).toInt());
        out.append(p);
    }
    prefs.endArray();
    return out;
}

void TransitionPresetStore::save(const QString &name, const Transition &t)
{
    if (name.trimmed().isEmpty()) return;
    auto presets = loadAll();
    // Replace if a preset with this name already exists, otherwise append.
    bool replaced = false;
    for (auto &p : presets) {
        if (p.name == name) { p.transition = t; replaced = true; break; }
    }
    if (!replaced) {
        TransitionPreset p;
        p.name = name;
        p.transition = t;
        presets.append(p);
    }
    QSettings prefs("VSimpleEditor", "Preferences");
    prefs.beginWriteArray("transitionPresets");
    for (int i = 0; i < presets.size(); ++i) {
        prefs.setArrayIndex(i);
        prefs.setValue("name", presets[i].name);
        prefs.setValue("type", static_cast<int>(presets[i].transition.type));
        prefs.setValue("duration", presets[i].transition.duration);
        prefs.setValue("alignment", static_cast<int>(presets[i].transition.alignment));
        prefs.setValue("easing", static_cast<int>(presets[i].transition.easing));
    }
    prefs.endArray();
}

void TransitionPresetStore::remove(const QString &name)
{
    auto presets = loadAll();
    QList<TransitionPreset> kept;
    for (const auto &p : presets) {
        if (p.name != name) kept.append(p);
    }
    if (kept.size() == presets.size()) return; // nothing removed
    QSettings prefs("VSimpleEditor", "Preferences");
    // Clear the entire array first — beginWriteArray with a smaller size
    // doesn't truncate stale entries on some Qt versions.
    prefs.remove("transitionPresets");
    prefs.beginWriteArray("transitionPresets");
    for (int i = 0; i < kept.size(); ++i) {
        prefs.setArrayIndex(i);
        prefs.setValue("name", kept[i].name);
        prefs.setValue("type", static_cast<int>(kept[i].transition.type));
        prefs.setValue("duration", kept[i].transition.duration);
        prefs.setValue("alignment", static_cast<int>(kept[i].transition.alignment));
        prefs.setValue("easing", static_cast<int>(kept[i].transition.easing));
    }
    prefs.endArray();
}

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

    auto applyColorButtonStyle = [](QPushButton *btn, const QColor &c) {
        btn->setStyleSheet(QString("background-color: %1;").arg(c.name()));
    };

    // --- Drop shadow ---
    auto *shadowBox = new QGroupBox(QString::fromUtf8("\xe3\x83\x89\xe3\x83\xad\xe3\x83\x83\xe3\x83\x97\xe3\x82\xb7\xe3\x83\xa3\xe3\x83\x89\xe3\x82\xa6"), this); // ドロップシャドウ
    auto *shadowForm = new QFormLayout(shadowBox);
    m_shadowEnabled = new QCheckBox(QString::fromUtf8("\xe6\x9c\x89\xe5\x8a\xb9"), shadowBox); // 有効
    shadowForm->addRow(m_shadowEnabled);
    m_shadowOffX = new QDoubleSpinBox(shadowBox);
    m_shadowOffX->setRange(-200.0, 200.0); m_shadowOffX->setValue(4.0); m_shadowOffX->setSingleStep(0.5);
    m_shadowOffY = new QDoubleSpinBox(shadowBox);
    m_shadowOffY->setRange(-200.0, 200.0); m_shadowOffY->setValue(4.0); m_shadowOffY->setSingleStep(0.5);
    auto *shadowOffLayout = new QHBoxLayout();
    shadowOffLayout->addWidget(new QLabel("X:", shadowBox)); shadowOffLayout->addWidget(m_shadowOffX);
    shadowOffLayout->addWidget(new QLabel("Y:", shadowBox)); shadowOffLayout->addWidget(m_shadowOffY);
    shadowForm->addRow(QString::fromUtf8("\xe3\x82\xaa\xe3\x83\x95\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88:"), shadowOffLayout); // オフセット
    m_shadowBlur = new QDoubleSpinBox(shadowBox);
    m_shadowBlur->setRange(0.0, 64.0); m_shadowBlur->setValue(6.0); m_shadowBlur->setSingleStep(0.5);
    shadowForm->addRow(QString::fromUtf8("\xe3\x83\x96\xe3\x83\xa9\xe3\x83\xbc:"), m_shadowBlur); // ブラー
    m_shadowColorBtn = new QPushButton(QStringLiteral("Color"), shadowBox);
    applyColorButtonStyle(m_shadowColorBtn, m_shadowColor);
    connect(m_shadowColorBtn, &QPushButton::clicked, this, [this, applyColorButtonStyle]() {
        QColor c = QColorDialog::getColor(m_shadowColor, this,
            QString::fromUtf8("\xe3\x82\xb7\xe3\x83\xa3\xe3\x83\x89\xe3\x82\xa6\xe8\x89\xb2"), // シャドウ色
            QColorDialog::ShowAlphaChannel);
        if (c.isValid()) { m_shadowColor = c; applyColorButtonStyle(m_shadowColorBtn, c); }
    });
    shadowForm->addRow(QString::fromUtf8("\xe8\x89\xb2:"), m_shadowColorBtn); // 色
    m_shadowOpacity = new QDoubleSpinBox(shadowBox);
    m_shadowOpacity->setRange(0.0, 1.0); m_shadowOpacity->setValue(0.8); m_shadowOpacity->setSingleStep(0.05);
    shadowForm->addRow(QString::fromUtf8("\xe4\xb8\x8d\xe9\x80\x8f\xe6\x98\x8e\xe5\xba\xa6:"), m_shadowOpacity); // 不透明度
    layout->addWidget(shadowBox);

    // --- Outer glow ---
    auto *glowBox = new QGroupBox(QString::fromUtf8("\xe3\x82\xa2\xe3\x82\xa6\xe3\x82\xbf\xe3\x83\xbc\xe3\x83\xbb\xe3\x82\xb0\xe3\x83\xad\xe3\x83\xbc"), this); // アウター・グロー
    auto *glowForm = new QFormLayout(glowBox);
    m_glowEnabled = new QCheckBox(QString::fromUtf8("\xe6\x9c\x89\xe5\x8a\xb9"), glowBox);
    glowForm->addRow(m_glowEnabled);
    m_glowRadius = new QDoubleSpinBox(glowBox);
    m_glowRadius->setRange(0.0, 64.0); m_glowRadius->setValue(8.0); m_glowRadius->setSingleStep(0.5);
    glowForm->addRow(QString::fromUtf8("\xe5\x8d\x8a\xe5\xbe\x84:"), m_glowRadius); // 半径
    m_glowColorBtn = new QPushButton(QStringLiteral("Color"), glowBox);
    applyColorButtonStyle(m_glowColorBtn, m_glowColor);
    connect(m_glowColorBtn, &QPushButton::clicked, this, [this, applyColorButtonStyle]() {
        QColor c = QColorDialog::getColor(m_glowColor, this,
            QString::fromUtf8("\xe3\x82\xb0\xe3\x83\xad\xe3\x83\xbc\xe8\x89\xb2"), // グロー色
            QColorDialog::ShowAlphaChannel);
        if (c.isValid()) { m_glowColor = c; applyColorButtonStyle(m_glowColorBtn, c); }
    });
    glowForm->addRow(QString::fromUtf8("\xe8\x89\xb2:"), m_glowColorBtn);
    m_glowOpacity = new QDoubleSpinBox(glowBox);
    m_glowOpacity->setRange(0.0, 1.0); m_glowOpacity->setValue(0.9); m_glowOpacity->setSingleStep(0.05);
    glowForm->addRow(QString::fromUtf8("\xe4\xb8\x8d\xe9\x80\x8f\xe6\x98\x8e\xe5\xba\xa6:"), m_glowOpacity);
    layout->addWidget(glowBox);

    // --- Outline (stroke) ---
    auto *outlineBox = new QGroupBox(QString::fromUtf8("\xe3\x82\xa2\xe3\x82\xa6\xe3\x83\x88\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\xb3"), this); // アウトライン
    auto *outlineForm = new QFormLayout(outlineBox);
    m_outlineEnabled = new QCheckBox(QString::fromUtf8("\xe6\x9c\x89\xe5\x8a\xb9"), outlineBox);
    m_outlineEnabled->setChecked(true); // historical default — outline was always drawn when width>0
    outlineForm->addRow(m_outlineEnabled);
    m_outlineWidth = new QSpinBox(outlineBox);
    m_outlineWidth->setRange(0, 32); m_outlineWidth->setValue(2);
    outlineForm->addRow(QString::fromUtf8("\xe5\xb9\x85:"), m_outlineWidth); // 幅
    m_outlineColorBtn = new QPushButton(QStringLiteral("Color"), outlineBox);
    applyColorButtonStyle(m_outlineColorBtn, m_outlineColor);
    connect(m_outlineColorBtn, &QPushButton::clicked, this, [this, applyColorButtonStyle]() {
        QColor c = QColorDialog::getColor(m_outlineColor, this,
            QString::fromUtf8("\xe3\x82\xa2\xe3\x82\xa6\xe3\x83\x88\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\xb3\xe8\x89\xb2"), // アウトライン色
            QColorDialog::ShowAlphaChannel);
        if (c.isValid()) { m_outlineColor = c; applyColorButtonStyle(m_outlineColorBtn, c); }
    });
    outlineForm->addRow(QString::fromUtf8("\xe8\x89\xb2:"), m_outlineColorBtn);
    layout->addWidget(outlineBox);

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
        // Effect groups — disabled groups still write their config so a
        // toggle doesn't lose the user's tuning, only the bool flips.
        m_result.shadow        = m_shadowEnabled->isChecked();
        m_result.shadowOffsetX = m_shadowOffX->value();
        m_result.shadowOffsetY = m_shadowOffY->value();
        m_result.shadowBlur    = m_shadowBlur->value();
        m_result.shadowColor   = m_shadowColor;
        m_result.shadowOpacity = m_shadowOpacity->value();
        m_result.glow          = m_glowEnabled->isChecked();
        m_result.glowRadius    = m_glowRadius->value();
        m_result.glowColor     = m_glowColor;
        m_result.glowOpacity   = m_glowOpacity->value();
        // Outline width=0 disables the existing renderer path; honour the
        // explicit checkbox by zeroing width when the user unchecks it.
        m_result.outlineWidth  = m_outlineEnabled->isChecked() ? m_outlineWidth->value() : 0;
        m_result.outlineColor  = m_outlineColor;
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

void TransitionDialog::refreshPresetCombo()
{
    if (!m_presetCombo) return;
    m_presetCombo->blockSignals(true);
    m_presetCombo->clear();
    m_presetCombo->addItem(QStringLiteral("(プリセットを選択)"), QString());
    for (const auto &p : TransitionPresetStore::loadAll()) {
        m_presetCombo->addItem(p.name, p.name);
    }
    m_presetCombo->blockSignals(false);
}

void TransitionDialog::setupUI()
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    // Preset row first — it lets the user load + apply a saved combo
    // without touching the rest of the form. Save-as button next to it
    // captures the current Type/Duration/Alignment/Easing as a new preset.
    m_presetCombo = new QComboBox(this);
    auto *saveBtn = new QPushButton(QStringLiteral("名前を付けて保存..."), this);
    auto *delBtn  = new QPushButton(QStringLiteral("削除"), this);
    auto *presetRow = new QHBoxLayout();
    presetRow->addWidget(m_presetCombo, 1);
    presetRow->addWidget(saveBtn);
    presetRow->addWidget(delBtn);
    form->addRow(QStringLiteral("プリセット:"), presetRow);

    m_typeCombo = new QComboBox(this);
    // Display order grouped by family. Labels come from Transition::typeName
    // so a rename in Overlay.h flows here without a parallel edit.
    static const TransitionType kDialogTypeOrder[] = {
        TransitionType::FadeIn, TransitionType::FadeOut,
        TransitionType::CrossDissolve,
        TransitionType::DipToBlack, TransitionType::DipToWhite,
        TransitionType::WipeLeft, TransitionType::WipeRight,
        TransitionType::WipeUp, TransitionType::WipeDown,
        TransitionType::ClockWipe,
        TransitionType::BarnDoorHorizontal, TransitionType::BarnDoorVertical,
        TransitionType::IrisRound, TransitionType::IrisBox,
        TransitionType::SlideLeft, TransitionType::SlideRight,
        TransitionType::SlideUp, TransitionType::SlideDown,
        TransitionType::PushLeft, TransitionType::PushRight,
        TransitionType::PushUp, TransitionType::PushDown,
        TransitionType::CrossZoom, TransitionType::FilmDissolve,
        TransitionType::SpinCW, TransitionType::SpinCCW,
        TransitionType::DitherDissolve,
        TransitionType::IrisRoundClose, TransitionType::IrisBoxClose,
        TransitionType::BarnDoorHClose, TransitionType::BarnDoorVClose,
        TransitionType::ClockWipeCCW,
        TransitionType::WhipPanLeft, TransitionType::WhipPanRight,
        TransitionType::Glitch, TransitionType::LightLeak, TransitionType::MorphCut,
        TransitionType::FlipHorizontal, TransitionType::FlipVertical,
        TransitionType::LensFlare, TransitionType::FilmBurn,
        TransitionType::Pixelate, TransitionType::BlurDissolve,
        TransitionType::CameraShake, TransitionType::ColorChannelShift,
    };
    for (const TransitionType t : kDialogTypeOrder)
        m_typeCombo->addItem(t == TransitionType::MorphCut ? QStringLiteral("モーフカット") : Transition::typeName(t), static_cast<int>(t));
    form->addRow("Type:", m_typeCombo);

    m_durationSpin = new QDoubleSpinBox(this);
    m_durationSpin->setRange(0.1, 5.0);
    m_durationSpin->setValue(0.5);
    m_durationSpin->setSingleStep(0.1);
    m_durationSpin->setSuffix(" s");
    form->addRow("Duration:", m_durationSpin);

    m_alignmentCombo = new QComboBox(this);
    m_alignmentCombo->addItem("Center at Cut",
        static_cast<int>(TransitionAlignment::Center));
    m_alignmentCombo->addItem("Start at Cut",
        static_cast<int>(TransitionAlignment::Start));
    m_alignmentCombo->addItem("End at Cut",
        static_cast<int>(TransitionAlignment::End));
    form->addRow("Alignment:", m_alignmentCombo);

    m_easingCombo = new QComboBox(this);
    m_easingCombo->addItem("Linear",      static_cast<int>(TransitionEasing::Linear));
    m_easingCombo->addItem("Ease In",     static_cast<int>(TransitionEasing::EaseIn));
    m_easingCombo->addItem("Ease Out",    static_cast<int>(TransitionEasing::EaseOut));
    m_easingCombo->addItem("Ease In/Out", static_cast<int>(TransitionEasing::EaseInOut));
    form->addRow("Easing:", m_easingCombo);

    // Preset wiring (after the four field combos so we can read/write
    // them when applying or capturing a preset).
    refreshPresetCombo();
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int idx) {
            if (idx <= 0) return; // sentinel "(プリセットを選択)"
            const QString name = m_presetCombo->itemData(idx).toString();
            for (const auto &p : TransitionPresetStore::loadAll()) {
                if (p.name != name) continue;
                const int typeIdx = m_typeCombo->findData(static_cast<int>(p.transition.type));
                if (typeIdx >= 0) m_typeCombo->setCurrentIndex(typeIdx);
                m_durationSpin->setValue(p.transition.duration);
                const int aIdx = m_alignmentCombo->findData(static_cast<int>(p.transition.alignment));
                if (aIdx >= 0) m_alignmentCombo->setCurrentIndex(aIdx);
                const int eIdx = m_easingCombo->findData(static_cast<int>(p.transition.easing));
                if (eIdx >= 0) m_easingCombo->setCurrentIndex(eIdx);
                break;
            }
        });
    connect(saveBtn, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this,
            QStringLiteral("プリセット保存"),
            QStringLiteral("プリセット名:"),
            QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        Transition t;
        t.type = static_cast<TransitionType>(m_typeCombo->currentData().toInt());
        t.duration = m_durationSpin->value();
        t.alignment = static_cast<TransitionAlignment>(m_alignmentCombo->currentData().toInt());
        t.easing = static_cast<TransitionEasing>(m_easingCombo->currentData().toInt());
        TransitionPresetStore::save(name.trimmed(), t);
        refreshPresetCombo();
        const int idx = m_presetCombo->findData(name.trimmed());
        if (idx >= 0) m_presetCombo->setCurrentIndex(idx);
    });
    connect(delBtn, &QPushButton::clicked, this, [this]() {
        const int idx = m_presetCombo->currentIndex();
        if (idx <= 0) return;
        const QString name = m_presetCombo->itemData(idx).toString();
        if (QMessageBox::question(this,
                QStringLiteral("プリセット削除"),
                QStringLiteral("プリセット「%1」を削除しますか?").arg(name))
            != QMessageBox::Yes) return;
        TransitionPresetStore::remove(name);
        refreshPresetCombo();
    });

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_result.type = static_cast<TransitionType>(m_typeCombo->currentData().toInt());
        m_result.duration = m_durationSpin->value();
        m_result.alignment = static_cast<TransitionAlignment>(
            m_alignmentCombo->currentData().toInt());
        m_result.easing = static_cast<TransitionEasing>(
            m_easingCombo->currentData().toInt());
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
