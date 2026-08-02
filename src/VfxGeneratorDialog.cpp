#include "VfxGeneratorDialog.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <type_traits>

namespace {

int typeIndex(VfxGeneratorType type)
{
    const int index = static_cast<int>(type);
    const int count = static_cast<int>(VfxGeneratorType::Count);
    return index >= 0 && index < count ? index : 0;
}

} // namespace

VfxGeneratorDialog::VfxGeneratorDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("VFX ジェネレータ"));
    setModal(true);
    resize(820, 600);
    buildUi();

    m_elapsed.start();
    m_previewTimer = new QTimer(this);
    m_previewTimer->setInterval(33);
    connect(m_previewTimer, &QTimer::timeout,
            this, &VfxGeneratorDialog::updatePreview);
    m_previewTimer->start();
    updatePreview();
}

VfxGeneratorType VfxGeneratorDialog::generatorType() const
{
    if (!m_typeCombo)
        return VfxGeneratorType::Explosion;
    return static_cast<VfxGeneratorType>(m_typeCombo->currentData().toInt());
}

VfxGeneratorParameters VfxGeneratorDialog::parameters() const
{
    const VfxGeneratorType type = generatorType();
    VfxGeneratorParameters result = VfxGenerators::defaultParameters(type);
    const PageControls &page = pageFor(type);

    std::visit([&](auto &p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, ExplosionParameters>) {
            p.center = QPointF(doubleValue(page, QStringLiteral("centerX"), p.center.x()),
                               doubleValue(page, QStringLiteral("centerY"), p.center.y()));
            p.scale = doubleValue(page, QStringLiteral("scale"), p.scale);
            p.fragmentCount = intValue(page, QStringLiteral("fragmentCount"), p.fragmentCount);
            p.gravity = doubleValue(page, QStringLiteral("gravity"), p.gravity);
            p.colorTemperature = doubleValue(page, QStringLiteral("colorTemperature"), p.colorTemperature);
            p.duration = doubleValue(page, QStringLiteral("duration"), p.duration);
            p.seed = static_cast<unsigned int>(intValue(page, QStringLiteral("seed"), static_cast<int>(p.seed)));
        } else if constexpr (std::is_same_v<T, LightningParameters>) {
            p.start = QPointF(doubleValue(page, QStringLiteral("startX"), p.start.x()),
                              doubleValue(page, QStringLiteral("startY"), p.start.y()));
            p.end = QPointF(doubleValue(page, QStringLiteral("endX"), p.end.x()),
                            doubleValue(page, QStringLiteral("endY"), p.end.y()));
            p.branchProbability = doubleValue(page, QStringLiteral("branchProbability"), p.branchProbability);
            p.recursionDepth = intValue(page, QStringLiteral("recursionDepth"), p.recursionDepth);
            p.jitterWidth = doubleValue(page, QStringLiteral("jitterWidth"), p.jitterWidth);
            p.coreWidth = doubleValue(page, QStringLiteral("coreWidth"), p.coreWidth);
            p.flickerRate = doubleValue(page, QStringLiteral("flickerRate"), p.flickerRate);
            p.flickerDepth = doubleValue(page, QStringLiteral("flickerDepth"), p.flickerDepth);
            p.seed = static_cast<unsigned int>(intValue(page, QStringLiteral("seed"), static_cast<int>(p.seed)));
        } else if constexpr (std::is_same_v<T, ShockWaveParameters>) {
            p.center = QPointF(doubleValue(page, QStringLiteral("centerX"), p.center.x()),
                               doubleValue(page, QStringLiteral("centerY"), p.center.y()));
            p.initialRadius = doubleValue(page, QStringLiteral("initialRadius"), p.initialRadius);
            p.speed = doubleValue(page, QStringLiteral("speed"), p.speed);
            p.ringWidth = doubleValue(page, QStringLiteral("ringWidth"), p.ringWidth);
            p.distortionStrength = doubleValue(page, QStringLiteral("distortionStrength"), p.distortionStrength);
            p.decay = doubleValue(page, QStringLiteral("decay"), p.decay);
        } else if constexpr (std::is_same_v<T, EnergyBeamParameters>) {
            p.start = QPointF(doubleValue(page, QStringLiteral("startX"), p.start.x()),
                              doubleValue(page, QStringLiteral("startY"), p.start.y()));
            p.end = QPointF(doubleValue(page, QStringLiteral("endX"), p.end.x()),
                            doubleValue(page, QStringLiteral("endY"), p.end.y()));
            p.coreWidth = doubleValue(page, QStringLiteral("coreWidth"), p.coreWidth);
            p.haloWidth = doubleValue(page, QStringLiteral("haloWidth"), p.haloWidth);
            p.noiseIntensity = doubleValue(page, QStringLiteral("noiseIntensity"), p.noiseIntensity);
            p.flowSpeed = doubleValue(page, QStringLiteral("flowSpeed"), p.flowSpeed);
            p.seed = static_cast<unsigned int>(intValue(page, QStringLiteral("seed"), static_cast<int>(p.seed)));
        } else if constexpr (std::is_same_v<T, MagicCircleParameters>) {
            p.center = QPointF(doubleValue(page, QStringLiteral("centerX"), p.center.x()),
                               doubleValue(page, QStringLiteral("centerY"), p.center.y()));
            p.radius = doubleValue(page, QStringLiteral("radius"), p.radius);
            p.ringCount = qBound(1, intValue(page, QStringLiteral("ringCount"), p.ringCount), 8);
            p.segmentCount = intValue(page, QStringLiteral("segmentCount"), p.segmentCount);
            p.lineWidth = doubleValue(page, QStringLiteral("lineWidth"), p.lineWidth);
            p.tilt.rotationX = doubleValue(page, QStringLiteral("tiltX"), p.tilt.rotationX);
            p.tilt.rotationY = doubleValue(page, QStringLiteral("tiltY"), p.tilt.rotationY);
            const QVector<double> existingSpeeds = p.rotationSpeeds;
            p.rotationSpeeds.clear();
            for (int i = 0; i < p.ringCount; ++i) {
                p.rotationSpeeds.append(doubleValue(
                    page, QStringLiteral("speed%1").arg(i),
                    i < static_cast<int>(existingSpeeds.size()) ? existingSpeeds[i] : 0.4));
            }
            p.seed = static_cast<unsigned int>(intValue(page, QStringLiteral("seed"), static_cast<int>(p.seed)));
        } else if constexpr (std::is_same_v<T, MuzzleFlashParameters>) {
            p.center = QPointF(doubleValue(page, QStringLiteral("centerX"), p.center.x()),
                               doubleValue(page, QStringLiteral("centerY"), p.center.y()));
            p.directionDegrees = doubleValue(page, QStringLiteral("directionDegrees"), p.directionDegrees);
            p.scale = doubleValue(page, QStringLiteral("scale"), p.scale);
            p.spikeCount = intValue(page, QStringLiteral("spikeCount"), p.spikeCount);
            p.durationFrames = intValue(page, QStringLiteral("durationFrames"), p.durationFrames);
            p.frameRate = doubleValue(page, QStringLiteral("frameRate"), p.frameRate);
            p.seed = static_cast<unsigned int>(intValue(page, QStringLiteral("seed"), static_cast<int>(p.seed)));
        } else if constexpr (std::is_same_v<T, EnergyShieldParameters>) {
            p.center = QPointF(doubleValue(page, QStringLiteral("centerX"), p.center.x()),
                               doubleValue(page, QStringLiteral("centerY"), p.center.y()));
            p.radius = doubleValue(page, QStringLiteral("radius"), p.radius);
            p.edgeSharpness = doubleValue(page, QStringLiteral("edgeSharpness"), p.edgeSharpness);
            p.cellSize = doubleValue(page, QStringLiteral("cellSize"), p.cellSize);
            p.rippleStrength = doubleValue(page, QStringLiteral("rippleStrength"), p.rippleStrength);
            p.rippleSpeed = doubleValue(page, QStringLiteral("rippleSpeed"), p.rippleSpeed);
            const int impactCount = qBound(0,
                intValue(page, QStringLiteral("impactCount"),
                         static_cast<int>(p.impactPoints.size())), 8);
            p.impactPoints.clear();
            for (int i = 0; i < impactCount; ++i) {
                p.impactPoints.append(QPointF(
                    doubleValue(page, QStringLiteral("impact%1X").arg(i), p.center.x()),
                    doubleValue(page, QStringLiteral("impact%1Y").arg(i), p.center.y())));
            }
            p.seed = static_cast<unsigned int>(intValue(page, QStringLiteral("seed"), static_cast<int>(p.seed)));
        }
    }, result);

    if (page.colorButton)
        std::visit([&](auto &p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, LightningParameters>
                          || std::is_same_v<T, ShockWaveParameters>
                          || std::is_same_v<T, EnergyBeamParameters>
                          || std::is_same_v<T, MagicCircleParameters>
                          || std::is_same_v<T, EnergyShieldParameters>)
                p.color = page.color;
        }, result);
    return result;
}

void VfxGeneratorDialog::setGeneratorType(VfxGeneratorType type)
{
    if (!m_typeCombo)
        return;
    const int index = m_typeCombo->findData(static_cast<int>(type));
    if (index >= 0)
        m_typeCombo->setCurrentIndex(index);
}

void VfxGeneratorDialog::setParameters(VfxGeneratorType type,
                                       const VfxGeneratorParameters &parameters)
{
    setGeneratorType(type);
    PageControls &page = pageFor(type);
    std::visit([&](const auto &p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, ExplosionParameters>) {
            setPageValue(page, QStringLiteral("centerX"), p.center.x());
            setPageValue(page, QStringLiteral("centerY"), p.center.y());
            setPageValue(page, QStringLiteral("scale"), p.scale);
            setPageValue(page, QStringLiteral("fragmentCount"), p.fragmentCount);
            setPageValue(page, QStringLiteral("gravity"), p.gravity);
            setPageValue(page, QStringLiteral("colorTemperature"), p.colorTemperature);
            setPageValue(page, QStringLiteral("duration"), p.duration);
            setPageValue(page, QStringLiteral("seed"), static_cast<int>(p.seed));
        } else if constexpr (std::is_same_v<T, LightningParameters>) {
            setPageValue(page, QStringLiteral("startX"), p.start.x());
            setPageValue(page, QStringLiteral("startY"), p.start.y());
            setPageValue(page, QStringLiteral("endX"), p.end.x());
            setPageValue(page, QStringLiteral("endY"), p.end.y());
            setPageValue(page, QStringLiteral("branchProbability"), p.branchProbability);
            setPageValue(page, QStringLiteral("recursionDepth"), p.recursionDepth);
            setPageValue(page, QStringLiteral("jitterWidth"), p.jitterWidth);
            setPageValue(page, QStringLiteral("coreWidth"), p.coreWidth);
            setPageValue(page, QStringLiteral("flickerRate"), p.flickerRate);
            setPageValue(page, QStringLiteral("flickerDepth"), p.flickerDepth);
            setPageValue(page, QStringLiteral("seed"), static_cast<int>(p.seed));
            page.color = p.color;
        } else if constexpr (std::is_same_v<T, ShockWaveParameters>) {
            setPageValue(page, QStringLiteral("centerX"), p.center.x());
            setPageValue(page, QStringLiteral("centerY"), p.center.y());
            setPageValue(page, QStringLiteral("initialRadius"), p.initialRadius);
            setPageValue(page, QStringLiteral("speed"), p.speed);
            setPageValue(page, QStringLiteral("ringWidth"), p.ringWidth);
            setPageValue(page, QStringLiteral("distortionStrength"), p.distortionStrength);
            setPageValue(page, QStringLiteral("decay"), p.decay);
            page.color = p.color;
        } else if constexpr (std::is_same_v<T, EnergyBeamParameters>) {
            setPageValue(page, QStringLiteral("startX"), p.start.x());
            setPageValue(page, QStringLiteral("startY"), p.start.y());
            setPageValue(page, QStringLiteral("endX"), p.end.x());
            setPageValue(page, QStringLiteral("endY"), p.end.y());
            setPageValue(page, QStringLiteral("coreWidth"), p.coreWidth);
            setPageValue(page, QStringLiteral("haloWidth"), p.haloWidth);
            setPageValue(page, QStringLiteral("noiseIntensity"), p.noiseIntensity);
            setPageValue(page, QStringLiteral("flowSpeed"), p.flowSpeed);
            setPageValue(page, QStringLiteral("seed"), static_cast<int>(p.seed));
            page.color = p.color;
        } else if constexpr (std::is_same_v<T, MagicCircleParameters>) {
            setPageValue(page, QStringLiteral("centerX"), p.center.x());
            setPageValue(page, QStringLiteral("centerY"), p.center.y());
            setPageValue(page, QStringLiteral("radius"), p.radius);
            setPageValue(page, QStringLiteral("ringCount"), p.ringCount);
            setPageValue(page, QStringLiteral("segmentCount"), p.segmentCount);
            setPageValue(page, QStringLiteral("lineWidth"), p.lineWidth);
            setPageValue(page, QStringLiteral("tiltX"), p.tilt.rotationX);
            setPageValue(page, QStringLiteral("tiltY"), p.tilt.rotationY);
            for (int i = 0; i < 8; ++i) {
                const double fallback = (i % 2 == 0 ? 0.5 : -0.35)
                    / (1.0 + static_cast<double>(i) * 0.2);
                const double speed = i < static_cast<int>(p.rotationSpeeds.size())
                    ? p.rotationSpeeds[i] : fallback;
                setPageValue(page, QStringLiteral("speed%1").arg(i), speed);
            }
            setPageValue(page, QStringLiteral("seed"), static_cast<int>(p.seed));
            page.color = p.color;
        } else if constexpr (std::is_same_v<T, MuzzleFlashParameters>) {
            setPageValue(page, QStringLiteral("centerX"), p.center.x());
            setPageValue(page, QStringLiteral("centerY"), p.center.y());
            setPageValue(page, QStringLiteral("directionDegrees"), p.directionDegrees);
            setPageValue(page, QStringLiteral("scale"), p.scale);
            setPageValue(page, QStringLiteral("spikeCount"), p.spikeCount);
            setPageValue(page, QStringLiteral("durationFrames"), p.durationFrames);
            setPageValue(page, QStringLiteral("frameRate"), p.frameRate);
            setPageValue(page, QStringLiteral("seed"), static_cast<int>(p.seed));
        } else if constexpr (std::is_same_v<T, EnergyShieldParameters>) {
            setPageValue(page, QStringLiteral("centerX"), p.center.x());
            setPageValue(page, QStringLiteral("centerY"), p.center.y());
            setPageValue(page, QStringLiteral("radius"), p.radius);
            setPageValue(page, QStringLiteral("edgeSharpness"), p.edgeSharpness);
            setPageValue(page, QStringLiteral("cellSize"), p.cellSize);
            setPageValue(page, QStringLiteral("rippleStrength"), p.rippleStrength);
            setPageValue(page, QStringLiteral("rippleSpeed"), p.rippleSpeed);
            const int impactCount = qMin(8, static_cast<int>(p.impactPoints.size()));
            setPageValue(page, QStringLiteral("impactCount"), impactCount);
            for (int i = 0; i < 8; ++i) {
                const QPointF point = i < impactCount
                    ? p.impactPoints[i] : p.center;
                setPageValue(page, QStringLiteral("impact%1X").arg(i), point.x());
                setPageValue(page, QStringLiteral("impact%1Y").arg(i), point.y());
            }
            setPageValue(page, QStringLiteral("seed"), static_cast<int>(p.seed));
            page.color = p.color;
        }
    }, parameters);
    updateColorButton(page);
    restartPreview();
}

void VfxGeneratorDialog::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(10);

    auto *typeLayout = new QHBoxLayout;
    typeLayout->addWidget(new QLabel(QStringLiteral("ジェネレータ"), this));
    m_typeCombo = new QComboBox(this);
    for (VfxGeneratorType type : VfxGenerators::allTypes())
        m_typeCombo->addItem(VfxGenerators::displayName(type), static_cast<int>(type));
    typeLayout->addWidget(m_typeCombo, 1);
    mainLayout->addLayout(typeLayout);

    auto *bodyLayout = new QHBoxLayout;
    bodyLayout->setSpacing(12);

    auto *settingsScroll = new QScrollArea(this);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    m_pagesStack = new QStackedWidget(settingsScroll);
    settingsScroll->setWidget(m_pagesStack);
    bodyLayout->addWidget(settingsScroll, 3);

    auto *previewColumn = new QVBoxLayout;
    previewColumn->addWidget(new QLabel(QStringLiteral("プレビュー"), this));
    m_previewLabel = new QLabel(this);
    m_previewLabel->setMinimumSize(360, 220);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: #10141b; border: 1px solid #384454; color: #b8c2d1; }"));
    m_previewLabel->setText(QStringLiteral("生成結果を準備中"));
    previewColumn->addWidget(m_previewLabel);
    m_restartButton = new QPushButton(QStringLiteral("プレビューを再生"), this);
    m_restartButton->setToolTip(QStringLiteral("プレビュー時刻を 0 秒に戻します"));
    connect(m_restartButton, &QPushButton::clicked,
            this, &VfxGeneratorDialog::restartPreview);
    previewColumn->addWidget(m_restartButton);
    previewColumn->addStretch();
    bodyLayout->addLayout(previewColumn, 4);
    mainLayout->addLayout(bodyLayout, 1);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(m_buttons);

    const int pageCount = static_cast<int>(VfxGeneratorType::Count);
    m_pages.resize(pageCount);
    buildExplosionPage();
    buildLightningPage();
    buildShockWavePage();
    buildEnergyBeamPage();
    buildMagicCirclePage();
    buildMuzzleFlashPage();
    buildEnergyShieldPage();

    connect(m_typeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &VfxGeneratorDialog::onGeneratorChanged);
}

QDoubleSpinBox *VfxGeneratorDialog::addDouble(PageControls &page,
                                               const QString &name,
                                               const QString &label,
                                               double minValue, double maxValue,
                                               double value, double step)
{
    auto *spin = new QDoubleSpinBox(page.page);
    spin->setRange(minValue, maxValue);
    spin->setSingleStep(step);
    spin->setDecimals(3);
    spin->setValue(value);
    spin->setKeyboardTracking(false);
    page.form->addRow(label, spin);
    page.doubles.insert(name, spin);
    connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &VfxGeneratorDialog::updatePreview);
    return spin;
}

QSpinBox *VfxGeneratorDialog::addInt(PageControls &page, const QString &name,
                                     const QString &label, int minValue,
                                     int maxValue, int value)
{
    auto *spin = new QSpinBox(page.page);
    spin->setRange(minValue, maxValue);
    spin->setValue(value);
    spin->setKeyboardTracking(false);
    page.form->addRow(label, spin);
    page.ints.insert(name, spin);
    connect(spin, qOverload<int>(&QSpinBox::valueChanged),
            this, &VfxGeneratorDialog::updatePreview);
    return spin;
}

void VfxGeneratorDialog::addColor(PageControls &page, const QColor &color)
{
    page.color = color;
    page.colorButton = new QPushButton(page.page);
    page.colorButton->setText(QStringLiteral("色を選択"));
    page.colorButton->setAccessibleName(QStringLiteral("VFX カラー"));
    page.form->addRow(QStringLiteral("Color"), page.colorButton);
    connect(page.colorButton, &QPushButton::clicked,
            this, &VfxGeneratorDialog::chooseColor);
    updateColorButton(page);
}

void VfxGeneratorDialog::buildExplosionPage()
{
    PageControls &page = m_pages[typeIndex(VfxGeneratorType::Explosion)];
    page.page = new QWidget(m_pagesStack);
    page.form = new QFormLayout(page.page);
    page.form->setContentsMargins(8, 8, 8, 8);
    page.form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const ExplosionParameters p;
    addDouble(page, QStringLiteral("centerX"), QStringLiteral("中心 X"), 0.0, 1.0, p.center.x());
    addDouble(page, QStringLiteral("centerY"), QStringLiteral("中心 Y"), 0.0, 1.0, p.center.y());
    addDouble(page, QStringLiteral("scale"), QStringLiteral("規模"), 0.01, 1.0, p.scale);
    addInt(page, QStringLiteral("fragmentCount"), QStringLiteral("破片数"), 0, 256, p.fragmentCount);
    addDouble(page, QStringLiteral("gravity"), QStringLiteral("重力"), 0.0, 1000.0, p.gravity, 1.0);
    addDouble(page, QStringLiteral("colorTemperature"), QStringLiteral("色温度"), 0.0, 1.0, p.colorTemperature);
    addDouble(page, QStringLiteral("duration"), QStringLiteral("継続時間 (秒)"), 0.05, 10.0, p.duration, 0.05);
    addInt(page, QStringLiteral("seed"), QStringLiteral("シード"), 0, 2147483647, static_cast<int>(p.seed));
    m_pagesStack->addWidget(page.page);
}

void VfxGeneratorDialog::buildLightningPage()
{
    PageControls &page = m_pages[typeIndex(VfxGeneratorType::Lightning)];
    page.page = new QWidget(m_pagesStack);
    page.form = new QFormLayout(page.page);
    page.form->setContentsMargins(8, 8, 8, 8);
    const LightningParameters p;
    addDouble(page, QStringLiteral("startX"), QStringLiteral("始点 X"), 0.0, 1.0, p.start.x());
    addDouble(page, QStringLiteral("startY"), QStringLiteral("始点 Y"), 0.0, 1.0, p.start.y());
    addDouble(page, QStringLiteral("endX"), QStringLiteral("終点 X"), 0.0, 1.0, p.end.x());
    addDouble(page, QStringLiteral("endY"), QStringLiteral("終点 Y"), 0.0, 1.0, p.end.y());
    addDouble(page, QStringLiteral("branchProbability"), QStringLiteral("分岐確率"), 0.0, 1.0, p.branchProbability);
    addInt(page, QStringLiteral("recursionDepth"), QStringLiteral("再帰深度"), 0, 8, p.recursionDepth);
    addDouble(page, QStringLiteral("jitterWidth"), QStringLiteral("ジッタ幅"), 0.0, 0.2, p.jitterWidth);
    addDouble(page, QStringLiteral("coreWidth"), QStringLiteral("芯の太さ"), 0.5, 20.0, p.coreWidth, 0.5);
    addDouble(page, QStringLiteral("flickerRate"), QStringLiteral("明滅速度"), 0.1, 30.0, p.flickerRate, 0.1);
    addDouble(page, QStringLiteral("flickerDepth"), QStringLiteral("明滅深度"), 0.0, 1.0, p.flickerDepth);
    addInt(page, QStringLiteral("seed"), QStringLiteral("シード"), 0, 2147483647, static_cast<int>(p.seed));
    addColor(page, p.color);
    m_pagesStack->addWidget(page.page);
}

void VfxGeneratorDialog::buildShockWavePage()
{
    PageControls &page = m_pages[typeIndex(VfxGeneratorType::ShockWave)];
    page.page = new QWidget(m_pagesStack);
    page.form = new QFormLayout(page.page);
    page.form->setContentsMargins(8, 8, 8, 8);
    const ShockWaveParameters p;
    addDouble(page, QStringLiteral("centerX"), QStringLiteral("中心 X"), 0.0, 1.0, p.center.x());
    addDouble(page, QStringLiteral("centerY"), QStringLiteral("中心 Y"), 0.0, 1.0, p.center.y());
    addDouble(page, QStringLiteral("initialRadius"), QStringLiteral("初期半径"), 0.0, 300.0, p.initialRadius, 1.0);
    addDouble(page, QStringLiteral("speed"), QStringLiteral("速度 (px/s)"), 0.0, 2000.0, p.speed, 10.0);
    addDouble(page, QStringLiteral("ringWidth"), QStringLiteral("リング幅"), 1.0, 100.0, p.ringWidth, 1.0);
    addDouble(page, QStringLiteral("distortionStrength"), QStringLiteral("歪み強度"), 0.0, 100.0, p.distortionStrength, 1.0);
    addDouble(page, QStringLiteral("decay"), QStringLiteral("減衰"), 0.0, 10.0, p.decay);
    addColor(page, p.color);
    m_pagesStack->addWidget(page.page);
}

void VfxGeneratorDialog::buildEnergyBeamPage()
{
    PageControls &page = m_pages[typeIndex(VfxGeneratorType::EnergyBeam)];
    page.page = new QWidget(m_pagesStack);
    page.form = new QFormLayout(page.page);
    page.form->setContentsMargins(8, 8, 8, 8);
    const EnergyBeamParameters p;
    addDouble(page, QStringLiteral("startX"), QStringLiteral("始点 X"), 0.0, 1.0, p.start.x());
    addDouble(page, QStringLiteral("startY"), QStringLiteral("始点 Y"), 0.0, 1.0, p.start.y());
    addDouble(page, QStringLiteral("endX"), QStringLiteral("終点 X"), 0.0, 1.0, p.end.x());
    addDouble(page, QStringLiteral("endY"), QStringLiteral("終点 Y"), 0.0, 1.0, p.end.y());
    addDouble(page, QStringLiteral("coreWidth"), QStringLiteral("芯幅"), 0.5, 50.0, p.coreWidth, 0.5);
    addDouble(page, QStringLiteral("haloWidth"), QStringLiteral("ハロー幅"), 1.0, 200.0, p.haloWidth, 1.0);
    addDouble(page, QStringLiteral("noiseIntensity"), QStringLiteral("ノイズ強度"), 0.0, 1.0, p.noiseIntensity);
    addDouble(page, QStringLiteral("flowSpeed"), QStringLiteral("流れ速度"), 0.0, 20.0, p.flowSpeed, 0.1);
    addInt(page, QStringLiteral("seed"), QStringLiteral("シード"), 0, 2147483647, static_cast<int>(p.seed));
    addColor(page, p.color);
    m_pagesStack->addWidget(page.page);
}

void VfxGeneratorDialog::buildMagicCirclePage()
{
    PageControls &page = m_pages[typeIndex(VfxGeneratorType::MagicCircle)];
    page.page = new QWidget(m_pagesStack);
    page.form = new QFormLayout(page.page);
    page.form->setContentsMargins(8, 8, 8, 8);
    const MagicCircleParameters p;
    addDouble(page, QStringLiteral("centerX"), QStringLiteral("中心 X"), 0.0, 1.0, p.center.x());
    addDouble(page, QStringLiteral("centerY"), QStringLiteral("中心 Y"), 0.0, 1.0, p.center.y());
    addDouble(page, QStringLiteral("radius"), QStringLiteral("半径"), 0.05, 1.0, p.radius);
    addInt(page, QStringLiteral("ringCount"), QStringLiteral("リング数"), 1, 8, p.ringCount);
    addInt(page, QStringLiteral("segmentCount"), QStringLiteral("分割数"), 3, 64, p.segmentCount);
    addDouble(page, QStringLiteral("lineWidth"), QStringLiteral("線幅"), 0.5, 20.0, p.lineWidth, 0.5);
    addDouble(page, QStringLiteral("tiltX"), QStringLiteral("3D 傾き X"), -80.0, 80.0, p.tilt.rotationX, 1.0);
    addDouble(page, QStringLiteral("tiltY"), QStringLiteral("3D 傾き Y"), -80.0, 80.0, p.tilt.rotationY, 1.0);
    for (int i = 0; i < 8; ++i) {
        const double fallback = (i % 2 == 0 ? 0.5 : -0.35)
            / (1.0 + static_cast<double>(i) * 0.2);
        const double speed = i < static_cast<int>(p.rotationSpeeds.size())
            ? p.rotationSpeeds[i] : fallback;
        addDouble(page, QStringLiteral("speed%1").arg(i),
                  QStringLiteral("リング %1 回転速度").arg(i + 1),
                  -5.0, 5.0, speed, 0.05);
    }
    addInt(page, QStringLiteral("seed"), QStringLiteral("シード"), 0, 2147483647, static_cast<int>(p.seed));
    addColor(page, p.color);
    m_pagesStack->addWidget(page.page);
}

void VfxGeneratorDialog::buildMuzzleFlashPage()
{
    PageControls &page = m_pages[typeIndex(VfxGeneratorType::MuzzleFlash)];
    page.page = new QWidget(m_pagesStack);
    page.form = new QFormLayout(page.page);
    page.form->setContentsMargins(8, 8, 8, 8);
    const MuzzleFlashParameters p;
    addDouble(page, QStringLiteral("centerX"), QStringLiteral("中心 X"), 0.0, 1.0, p.center.x());
    addDouble(page, QStringLiteral("centerY"), QStringLiteral("中心 Y"), 0.0, 1.0, p.center.y());
    addDouble(page, QStringLiteral("directionDegrees"), QStringLiteral("方向 (度)"), -360.0, 360.0, p.directionDegrees, 1.0);
    addDouble(page, QStringLiteral("scale"), QStringLiteral("規模"), 0.05, 1.0, p.scale);
    addInt(page, QStringLiteral("spikeCount"), QStringLiteral("スパイク数"), 1, 64, p.spikeCount);
    addInt(page, QStringLiteral("durationFrames"), QStringLiteral("継続フレーム数"), 1, 10, p.durationFrames);
    addDouble(page, QStringLiteral("frameRate"), QStringLiteral("基準 fps"), 1.0, 240.0, p.frameRate, 1.0);
    addInt(page, QStringLiteral("seed"), QStringLiteral("シード"), 0, 2147483647, static_cast<int>(p.seed));
    m_pagesStack->addWidget(page.page);
}

void VfxGeneratorDialog::buildEnergyShieldPage()
{
    PageControls &page = m_pages[typeIndex(VfxGeneratorType::EnergyShield)];
    page.page = new QWidget(m_pagesStack);
    page.form = new QFormLayout(page.page);
    page.form->setContentsMargins(8, 8, 8, 8);
    const EnergyShieldParameters p;
    addDouble(page, QStringLiteral("centerX"), QStringLiteral("中心 X"), 0.0, 1.0, p.center.x());
    addDouble(page, QStringLiteral("centerY"), QStringLiteral("中心 Y"), 0.0, 1.0, p.center.y());
    addDouble(page, QStringLiteral("radius"), QStringLiteral("半径"), 0.05, 1.0, p.radius);
    addDouble(page, QStringLiteral("edgeSharpness"), QStringLiteral("縁の鋭さ"), 0.5, 12.0, p.edgeSharpness, 0.5);
    addDouble(page, QStringLiteral("cellSize"), QStringLiteral("セルサイズ"), 6.0, 120.0, p.cellSize, 1.0);
    addDouble(page, QStringLiteral("rippleStrength"), QStringLiteral("着弾波紋強度"), 0.0, 2.0, p.rippleStrength);
    addDouble(page, QStringLiteral("rippleSpeed"), QStringLiteral("波紋速度"), 1.0, 1000.0, p.rippleSpeed, 10.0);
    addInt(page, QStringLiteral("impactCount"), QStringLiteral("着弾点数"), 0, 8,
           static_cast<int>(p.impactPoints.size()));
    for (int i = 0; i < 8; ++i) {
        const QPointF point = i < static_cast<int>(p.impactPoints.size())
            ? p.impactPoints[i] : p.center;
        addDouble(page, QStringLiteral("impact%1X").arg(i),
                  QStringLiteral("着弾 %1 X").arg(i + 1), 0.0, 1.0,
                  point.x());
        addDouble(page, QStringLiteral("impact%1Y").arg(i),
                  QStringLiteral("着弾 %1 Y").arg(i + 1), 0.0, 1.0,
                  point.y());
    }
    addInt(page, QStringLiteral("seed"), QStringLiteral("シード"), 0, 2147483647, static_cast<int>(p.seed));
    addColor(page, p.color);
    m_pagesStack->addWidget(page.page);
}

VfxGeneratorDialog::PageControls &VfxGeneratorDialog::pageFor(VfxGeneratorType type)
{
    return m_pages[typeIndex(type)];
}

const VfxGeneratorDialog::PageControls &VfxGeneratorDialog::pageFor(
    VfxGeneratorType type) const
{
    return m_pages[typeIndex(type)];
}

void VfxGeneratorDialog::setPageValue(PageControls &page, const QString &name,
                                      double value)
{
    if (auto *spin = page.doubles.value(name, nullptr))
        spin->setValue(value);
}

void VfxGeneratorDialog::setPageValue(PageControls &page, const QString &name,
                                      int value)
{
    if (auto *spin = page.ints.value(name, nullptr))
        spin->setValue(value);
}

double VfxGeneratorDialog::doubleValue(const PageControls &page,
                                       const QString &name, double fallback) const
{
    const auto *spin = page.doubles.value(name, nullptr);
    return spin ? spin->value() : fallback;
}

int VfxGeneratorDialog::intValue(const PageControls &page, const QString &name,
                                 int fallback) const
{
    const auto *spin = page.ints.value(name, nullptr);
    return spin ? spin->value() : fallback;
}

void VfxGeneratorDialog::updateColorButton(PageControls &page)
{
    if (!page.colorButton)
        return;
    const QString colorName = page.color.name(QColor::HexArgb);
    page.colorButton->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: white; min-height: 28px; }"
        "QPushButton:focus { border: 2px solid #76b7ff; }").arg(colorName));
}

void VfxGeneratorDialog::onGeneratorChanged(int index)
{
    if (m_pagesStack && index >= 0 && index < m_pagesStack->count())
        m_pagesStack->setCurrentIndex(index);
    restartPreview();
}

void VfxGeneratorDialog::updatePreview()
{
    if (!m_previewLabel || !m_elapsed.isValid())
        return;
    const VfxGeneratorType type = generatorType();
    const VfxGeneratorParameters params = parameters();
    double cycle = VfxGenerators::durationSeconds(type, params);
    if (!std::isfinite(cycle) || cycle <= 0.0)
        cycle = 1.0;
    const double elapsed = static_cast<double>(m_elapsed.elapsed()) / 1000.0;
    const double time = std::fmod(elapsed, cycle);
    const QImage image = VfxGenerators::render(type, QSize(360, 220), params, time);
    if (image.isNull()) {
        m_previewLabel->setPixmap(QPixmap());
        m_previewLabel->setText(QStringLiteral("プレビューを生成できません"));
        return;
    }
    const QPixmap pixmap = QPixmap::fromImage(image);
    m_previewLabel->setText(QString());
    m_previewLabel->setPixmap(pixmap.scaled(
        m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void VfxGeneratorDialog::restartPreview()
{
    m_elapsed.restart();
    updatePreview();
}

void VfxGeneratorDialog::chooseColor()
{
    auto *button = qobject_cast<QPushButton *>(sender());
    if (!button)
        return;
    for (PageControls &page : m_pages) {
        if (page.colorButton != button)
            continue;
        const QColor selected = QColorDialog::getColor(page.color, this,
                                                        QStringLiteral("VFX カラー"),
                                                        QColorDialog::ShowAlphaChannel);
        if (selected.isValid()) {
            page.color = selected;
            updateColorButton(page);
            updatePreview();
        }
        return;
    }
}
