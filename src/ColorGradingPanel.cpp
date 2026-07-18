#include "ColorGradingPanel.h"
#include "ColorWheelWidget.h"
#include "CurveEditor.h"
#include "HueVsSatEditor.h"
#include "WbPick.h"
#include "Timeline.h"
#include "UndoManager.h"
#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QColorDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>

namespace {

QJsonArray curvePointListToJson(const QVector<QPointF> &points)
{
    QJsonArray arr;
    for (const QPointF &p : points) {
        QJsonArray pt;
        pt.append(p.x());
        pt.append(p.y());
        arr.append(pt);
    }
    return arr;
}

QVector<QPointF> curvePointListFromJson(const QJsonArray &arr)
{
    QVector<QPointF> points;
    points.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonArray pt = v.toArray();
        if (pt.size() < 2)
            continue;
        points.append(QPointF(pt.at(0).toDouble(), pt.at(1).toDouble()));
    }
    return points;
}

QJsonObject curveDataToJson(const ClipCurveData &curves, int srcWidth, int srcHeight)
{
    QJsonObject obj;
    obj[QStringLiteral("srcWidth")] = srcWidth;
    obj[QStringLiteral("srcHeight")] = srcHeight;
    if (!curves.hasCurves())
        return obj;

    QJsonArray channels;
    const QVector<QVector<QPointF>> points = curves.editorPointsOrIdentity();
    for (int ch = 0; ch < ClipCurveData::ChannelCount; ++ch)
        channels.append(curvePointListToJson(points.value(ch)));
    obj[QStringLiteral("channels")] = channels;
    return obj;
}

ClipCurveData curveDataFromJson(const QJsonObject &obj)
{
    ClipCurveData curves;
    const QJsonArray channels = obj.value(QStringLiteral("channels")).toArray();
    QVector<QVector<QPointF>> points;
    points.reserve(ClipCurveData::ChannelCount);
    for (int ch = 0; ch < ClipCurveData::ChannelCount; ++ch) {
        const QJsonArray channel =
            ch < channels.size() ? channels.at(ch).toArray() : QJsonArray{};
        points.append(curvePointListFromJson(channel));
    }
    curves.setPoints(points);
    return curves;
}

Timeline *findTimelineForColorPanel(QObject *panel)
{
    for (QObject *p = panel; p; p = p->parent()) {
        if (Timeline *timeline = p->findChild<Timeline *>())
            return timeline;
    }
    const auto topLevels = QApplication::topLevelWidgets();
    for (QWidget *w : topLevels) {
        if (Timeline *timeline = w->findChild<Timeline *>())
            return timeline;
    }
    return nullptr;
}

bool selectedClipIndex(Timeline *timeline, int *trackIdx, int *clipIdx)
{
    if (!timeline)
        return false;
    const QVector<TimelineTrack *> &tracks = timeline->videoTracks();
    for (int t = 0; t < tracks.size(); ++t) {
        const TimelineTrack *track = tracks[t];
        if (!track)
            continue;
        const int selected = track->selectedClip();
        if (selected < 0 || selected >= track->clips().size())
            continue;
        if (trackIdx)
            *trackIdx = t;
        if (clipIdx)
            *clipIdx = selected;
        return true;
    }
    return false;
}

ClipCurveData clipCurvesAt(Timeline *timeline, int trackIdx, int clipIdx)
{
    if (!timeline)
        return {};
    const QVector<TimelineTrack *> &tracks = timeline->videoTracks();
    if (trackIdx < 0 || trackIdx >= tracks.size() || !tracks[trackIdx])
        return {};
    const QVector<ClipInfo> &clips = tracks[trackIdx]->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return {};
    return clips[clipIdx].colorCurves;
}

HslSecondaryGrade clipHslSecondaryAt(Timeline *timeline, int trackIdx, int clipIdx)
{
    if (!timeline)
        return {};
    const QVector<TimelineTrack *> &tracks = timeline->videoTracks();
    if (trackIdx < 0 || trackIdx >= tracks.size() || !tracks[trackIdx])
        return {};
    const QVector<ClipInfo> &clips = tracks[trackIdx]->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return {};
    return clips[clipIdx].hslSecondary;
}

bool sameHslSecondary(const HslSecondaryGrade &a, const HslSecondaryGrade &b)
{
    auto near = [](double x, double y) { return std::abs(x - y) <= 1e-9; };
    return a.enabled == b.enabled
        && near(a.hueCenter, b.hueCenter)
        && near(a.hueRange, b.hueRange)
        && near(a.satMin, b.satMin)
        && near(a.satMax, b.satMax)
        && near(a.lumaMin, b.lumaMin)
        && near(a.lumaMax, b.lumaMax)
        && near(a.softness, b.softness)
        && near(a.liftR, b.liftR)
        && near(a.liftG, b.liftG)
        && near(a.liftB, b.liftB)
        && near(a.gammaR, b.gammaR)
        && near(a.gammaG, b.gammaG)
        && near(a.gammaB, b.gammaB)
        && near(a.gainR, b.gainR)
        && near(a.gainG, b.gainG)
        && near(a.gainB, b.gainB);
}

bool writeCurvesToSelectedClip(Timeline *timeline,
                               const QVector<QVector<QPointF>> &editorPoints)
{
    int trackIdx = -1;
    int clipIdx = -1;
    if (!selectedClipIndex(timeline, &trackIdx, &clipIdx))
        return false;

    TimelineTrack *track = timeline->videoTracks().value(trackIdx, nullptr);
    if (!track)
        return false;

    QVector<ClipInfo> clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return false;

    ClipCurveData curves;
    curves.setPoints(editorPoints);
    if (clips[clipIdx].colorCurves.editorPointsOrIdentity()
        == curves.editorPointsOrIdentity()) {
        return true;
    }

    clips[clipIdx].colorCurves = curves;
    track->setClips(clips);
    if (UndoManager *undo = timeline->undoManager())
        undo->saveState(timeline->currentState(), QStringLiteral("Clip RGB curves"));
    timeline->refreshPlaybackSequence();
    return true;
}

bool writeHslSecondaryToSelectedClip(Timeline *timeline,
                                     const HslSecondaryGrade &hsl)
{
    int trackIdx = -1;
    int clipIdx = -1;
    if (!selectedClipIndex(timeline, &trackIdx, &clipIdx))
        return false;

    TimelineTrack *track = timeline->videoTracks().value(trackIdx, nullptr);
    if (!track)
        return false;

    QVector<ClipInfo> clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size())
        return false;

    if (sameHslSecondary(clips[clipIdx].hslSecondary, hsl))
        return true;

    clips[clipIdx].hslSecondary = hsl;
    track->setClips(clips);
    if (UndoManager *undo = timeline->undoManager())
        undo->saveState(timeline->currentState(), QStringLiteral("Clip HSL secondary"));
    timeline->refreshPlaybackSequence();
    return true;
}

} // namespace

ColorGradingPanel::ColorGradingPanel(QWidget *parent)
    : QDockWidget(tr("カラーグレーディング"), parent)
{
    setObjectName("ColorGradingPanel");
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable
                | QDockWidget::DockWidgetFloatable);

    auto *scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *content = new QWidget;
    auto *mainLayout = new QVBoxLayout(content);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(8);

    // --- US-CG-2: White Balance Section (sits ABOVE the LGG wheels because
    // GLPreview applies uWb at the very top of the grade chain — Temperature
    // shifts in Kelvin, Tint pulls magenta(+) / green(-)). ---
    {
        auto *wbGroup = new QGroupBox(tr("ホワイトバランス"));
        auto *wbLayout = new QVBoxLayout(wbGroup);
        wbLayout->setSpacing(4);

        auto *tempRow = new QHBoxLayout;
        auto *tempLbl = new QLabel(tr("Temperature"));
        tempLbl->setMinimumWidth(70);
        tempRow->addWidget(tempLbl);
        m_wbTemperature = new QSlider(Qt::Horizontal);
        m_wbTemperature->setObjectName(QStringLiteral("wbTemperature"));
        m_wbTemperature->setRange(-100, 100);
        m_wbTemperature->setValue(0);
        tempRow->addWidget(m_wbTemperature, 1);
        m_wbTemperatureLabel = new QLabel(tr("5500K"));
        m_wbTemperatureLabel->setMinimumWidth(55);
        m_wbTemperatureLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        tempRow->addWidget(m_wbTemperatureLabel);
        wbLayout->addLayout(tempRow);

        auto *tintRow = new QHBoxLayout;
        auto *tintLbl = new QLabel(tr("Tint"));
        tintLbl->setMinimumWidth(70);
        tintRow->addWidget(tintLbl);
        m_wbTint = new QSlider(Qt::Horizontal);
        m_wbTint->setObjectName(QStringLiteral("wbTint"));
        m_wbTint->setRange(-100, 100);
        m_wbTint->setValue(0);
        tintRow->addWidget(m_wbTint, 1);
        m_wbTintLabel = new QLabel(tr("+0"));
        m_wbTintLabel->setMinimumWidth(55);
        m_wbTintLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        tintRow->addWidget(m_wbTintLabel);
        wbLayout->addLayout(tintRow);

        mainLayout->addWidget(wbGroup);

        connect(m_wbTemperature, &QSlider::valueChanged,
                this, &ColorGradingPanel::onWhiteBalanceChanged);
        connect(m_wbTint, &QSlider::valueChanged,
                this, &ColorGradingPanel::onWhiteBalanceChanged);
    }

    // --- US-CG-3: Vignette / Power Window Section (sits below ホワイトバランス
    // because GLPreview applies the radial mask AFTER curves and BEFORE the
    // .cube LUT). Amount=0 is a free no-op (factor==1.0 in the shader). ---
    {
        auto *vigGroup = new QGroupBox(tr("ビネット"));
        auto *vigLayout = new QVBoxLayout(vigGroup);
        vigLayout->setSpacing(4);

        auto addVigRow = [&](const QString &name, int min, int max, int initial,
                             QSlider *&outSlider, QLabel *&outLabel) {
            auto *row = new QHBoxLayout;
            auto *lbl = new QLabel(name);
            lbl->setMinimumWidth(70);
            row->addWidget(lbl);
            outSlider = new QSlider(Qt::Horizontal);
            outSlider->setRange(min, max);
            outSlider->setValue(initial);
            row->addWidget(outSlider, 1);
            outLabel = new QLabel(QString::number(initial));
            outLabel->setMinimumWidth(55);
            outLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(outLabel);
            vigLayout->addLayout(row);
        };

        addVigRow(tr("Amount"),    -100, 100,  0, m_vigAmount,    m_vigAmountLabel);
        addVigRow(tr("Midpoint"),     0, 100, 70, m_vigMidpoint,  m_vigMidpointLabel);
        addVigRow(tr("Roundness"), -100, 100,  0, m_vigRoundness, m_vigRoundnessLabel);
        addVigRow(tr("Feather"),      0, 100, 30, m_vigFeather,   m_vigFeatherLabel);

        mainLayout->addWidget(vigGroup);

        connect(m_vigAmount,    &QSlider::valueChanged,
                this, &ColorGradingPanel::onVignetteChanged);
        connect(m_vigMidpoint,  &QSlider::valueChanged,
                this, &ColorGradingPanel::onVignetteChanged);
        connect(m_vigRoundness, &QSlider::valueChanged,
                this, &ColorGradingPanel::onVignetteChanged);
        connect(m_vigFeather,   &QSlider::valueChanged,
                this, &ColorGradingPanel::onVignetteChanged);
    }

    // --- US-EF-1: Chroma Key Section (Premiere Ultra Key / Resolve 3D Keyer
    // simplified). Applied at the very TOP of the compose path BEFORE WB so
    // the HSL gating + spill suppression operate on raw frame colour.
    // Default-disabled → bit-identical to a no-key state. ---
    {
        auto *chromaGroup = new QGroupBox(tr("クロマキー"));
        auto *chromaLayout = new QVBoxLayout(chromaGroup);
        chromaLayout->setSpacing(4);

        // Enable + key colour row
        auto *headerRow = new QHBoxLayout;
        m_chromaEnabled = new QCheckBox(tr("有効"));
        m_chromaEnabled->setChecked(false);
        headerRow->addWidget(m_chromaEnabled);

        auto *keyLbl = new QLabel(tr("キー色:"));
        headerRow->addWidget(keyLbl);
        m_chromaKeyColourBtn = new QPushButton;
        m_chromaKeyColourBtn->setMinimumWidth(60);
        m_chromaKeyColourBtn->setAutoFillBackground(true);
        m_chromaKeyColourBtn->setStyleSheet(
            QString("background-color: %1;").arg(m_chromaKey.name()));
        headerRow->addWidget(m_chromaKeyColourBtn, 1);
        chromaLayout->addLayout(headerRow);

        auto addChromaRow = [&](const QString &name, int min, int max, int initial,
                                QSlider *&outSlider, QLabel *&outLabel) {
            auto *row = new QHBoxLayout;
            auto *lbl = new QLabel(name);
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);
            outSlider = new QSlider(Qt::Horizontal);
            outSlider->setRange(min, max);
            outSlider->setValue(initial);
            row->addWidget(outSlider, 1);
            outLabel = new QLabel(QString::number(initial));
            outLabel->setMinimumWidth(45);
            outLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(outLabel);
            chromaLayout->addLayout(row);
        };

        addChromaRow(tr("Hue Tol"),  0, 180, 30, m_chromaHueTol,    m_chromaHueTolLabel);
        addChromaRow(tr("Sat Tol"),  0, 100, 60, m_chromaSatTol,    m_chromaSatTolLabel);
        addChromaRow(tr("Lum Tol"),  0, 100, 60, m_chromaLumTol,    m_chromaLumTolLabel);
        addChromaRow(tr("Spill"),    0, 100, 40, m_chromaSpill,     m_chromaSpillLabel);
        addChromaRow(tr("Softness"), 0,  50, 10, m_chromaSoftness,  m_chromaSoftnessLabel);

        mainLayout->addWidget(chromaGroup);

        connect(m_chromaEnabled,      &QCheckBox::toggled,
                this, &ColorGradingPanel::onChromaKeyChanged);
        connect(m_chromaKeyColourBtn, &QPushButton::clicked,
                this, &ColorGradingPanel::onChromaKeyColourClicked);
        connect(m_chromaHueTol,       &QSlider::valueChanged,
                this, &ColorGradingPanel::onChromaKeyChanged);
        connect(m_chromaSatTol,       &QSlider::valueChanged,
                this, &ColorGradingPanel::onChromaKeyChanged);
        connect(m_chromaLumTol,       &QSlider::valueChanged,
                this, &ColorGradingPanel::onChromaKeyChanged);
        connect(m_chromaSpill,        &QSlider::valueChanged,
                this, &ColorGradingPanel::onChromaKeyChanged);
        connect(m_chromaSoftness,     &QSlider::valueChanged,
                this, &ColorGradingPanel::onChromaKeyChanged);
    }

    // --- US-EF-2: Mask Animation Section (DaVinci Power Window simplified).
    // Sits BELOW クロマキー because the mask wraps the entire grade chain in
    // the GLPreview shader: ungraded outside, graded inside (invertible). A
    // disabled mask is a free no-op (the wrap branches around mix()). The
    // rect itself lives on this panel for now; per-clip keyframe storage +
    // linear interpolation are deferred to a follow-up (NIT-1). ---
    {
        auto *maskGroup = new QGroupBox(tr("マスク"));
        auto *maskLayout = new QVBoxLayout(maskGroup);
        maskLayout->setSpacing(4);

        // Row 1: enable + shape + invert
        auto *headerRow = new QHBoxLayout;
        m_maskEnabled = new QCheckBox(tr("有効"));
        m_maskEnabled->setChecked(false);
        headerRow->addWidget(m_maskEnabled);

        auto *shapeLbl = new QLabel(tr("形状:"));
        headerRow->addWidget(shapeLbl);
        m_maskShape = new QComboBox;
        m_maskShape->addItem(tr("矩形 (Rect)"));
        m_maskShape->addItem(tr("楕円 (Ellipse)"));
        m_maskShape->setCurrentIndex(0);
        headerRow->addWidget(m_maskShape, 1);

        m_maskInvert = new QCheckBox(tr("反転"));
        m_maskInvert->setChecked(false);
        headerRow->addWidget(m_maskInvert);
        maskLayout->addLayout(headerRow);

        // Row 2: feather slider [0..50] → 0..0.5 in normalized space.
        auto *featherRow = new QHBoxLayout;
        auto *featherLbl = new QLabel(tr("Feather"));
        featherLbl->setMinimumWidth(70);
        featherRow->addWidget(featherLbl);
        m_maskFeather = new QSlider(Qt::Horizontal);
        m_maskFeather->setRange(0, 50);
        m_maskFeather->setValue(m_maskFeatherSlider);
        featherRow->addWidget(m_maskFeather, 1);
        m_maskFeatherLabel = new QLabel(QString::number(m_maskFeatherSlider));
        m_maskFeatherLabel->setMinimumWidth(45);
        m_maskFeatherLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        featherRow->addWidget(m_maskFeatherLabel);
        maskLayout->addLayout(featherRow);

        // Row 3: rect read-out (4 labels — x, y, w, h normalized 0..1)
        auto *rectRow = new QHBoxLayout;
        rectRow->addWidget(new QLabel(tr("Rect:")));
        m_maskRectXLabel = new QLabel(QStringLiteral("x=0.25"));
        m_maskRectYLabel = new QLabel(QStringLiteral("y=0.25"));
        m_maskRectWLabel = new QLabel(QStringLiteral("w=0.50"));
        m_maskRectHLabel = new QLabel(QStringLiteral("h=0.50"));
        rectRow->addWidget(m_maskRectXLabel);
        rectRow->addWidget(m_maskRectYLabel);
        rectRow->addWidget(m_maskRectWLabel);
        rectRow->addWidget(m_maskRectHLabel);
        rectRow->addStretch();
        maskLayout->addLayout(rectRow);

        // Row 4: action buttons
        auto *btnRow = new QHBoxLayout;
        m_maskDrawBtn = new QPushButton(tr("マスクを描画"));
        btnRow->addWidget(m_maskDrawBtn);
        m_maskAddKeyframeBtn = new QPushButton(tr("キーフレーム追加"));
        btnRow->addWidget(m_maskAddKeyframeBtn);
        maskLayout->addLayout(btnRow);

        mainLayout->addWidget(maskGroup);

        connect(m_maskEnabled, &QCheckBox::toggled,
                this, &ColorGradingPanel::onMaskChanged);
        connect(m_maskShape,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { onMaskChanged(); });
        connect(m_maskInvert, &QCheckBox::toggled,
                this, &ColorGradingPanel::onMaskChanged);
        connect(m_maskFeather, &QSlider::valueChanged,
                this, &ColorGradingPanel::onMaskChanged);
        connect(m_maskDrawBtn, &QPushButton::clicked,
                this, &ColorGradingPanel::onMaskDrawClicked);
        connect(m_maskAddKeyframeBtn, &QPushButton::clicked,
                this, &ColorGradingPanel::onMaskAddKeyframeClicked);
    }

    // --- US-EF-3: HSL Qualifier Section (DaVinci secondary grading).
    // Isolates a hue/sat/luma range and applies a SECONDARY lift/gamma/gain
    // ONLY inside the qualified region. Sits BELOW マスク in the panel; the
    // GLPreview shader applies it AFTER chroma key and BEFORE WB so the
    // qualifier operates on raw frame colour. Default-disabled → free no-op. ---
    {
        auto *hslqGroup = new QGroupBox(tr("HSL クオリファイア"));
        auto *hslqLayout = new QVBoxLayout(hslqGroup);
        hslqLayout->setSpacing(4);

        // Row 1: enable
        auto *headerRow = new QHBoxLayout;
        m_hslqEnabled = new QCheckBox(tr("有効"));
        m_hslqEnabled->setChecked(false);
        headerRow->addWidget(m_hslqEnabled);
        headerRow->addStretch();
        hslqLayout->addLayout(headerRow);

        // Generic slider row helper for the qualifier — same layout idiom as
        // クロマキー so the visual cadence matches the other secondary stages.
        auto addHslqRow = [&](const QString &name, int min, int max, int initial,
                              const QString &suffix,
                              QSlider *&outSlider, QLabel *&outLabel) {
            auto *row = new QHBoxLayout;
            auto *lbl = new QLabel(name);
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);
            outSlider = new QSlider(Qt::Horizontal);
            outSlider->setRange(min, max);
            outSlider->setValue(initial);
            row->addWidget(outSlider, 1);
            outLabel = new QLabel(QString::number(initial) + suffix);
            outLabel->setMinimumWidth(45);
            outLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(outLabel);
            hslqLayout->addLayout(row);
        };

        // Qualifier axis controls — defaults target skin tones (hue=30°,
        // range=30°, sat 30..100%, luma 30..80%, softness 10).
        addHslqRow(tr("Hue Centre"), 0, 360, 30,  QStringLiteral("°"),
                   m_hslqHueCenter, m_hslqHueCenterLabel);
        addHslqRow(tr("Hue Range"),  0, 180, 30,  QStringLiteral("°"),
                   m_hslqHueRange,  m_hslqHueRangeLabel);
        addHslqRow(tr("Sat Min"),    0, 100, 30,  QStringLiteral("%"),
                   m_hslqSatMin,    m_hslqSatMinLabel);
        addHslqRow(tr("Sat Max"),    0, 100, 100, QStringLiteral("%"),
                   m_hslqSatMax,    m_hslqSatMaxLabel);
        addHslqRow(tr("Luma Min"),   0, 100, 30,  QStringLiteral("%"),
                   m_hslqLumaMin,   m_hslqLumaMinLabel);
        addHslqRow(tr("Luma Max"),   0, 100, 80,  QStringLiteral("%"),
                   m_hslqLumaMax,   m_hslqLumaMaxLabel);
        addHslqRow(tr("Softness"),   0,  50, 10,  QString(),
                   m_hslqSoftness,  m_hslqSoftnessLabel);

        // Per-stage adjustment — Lift [-50..+50] / Gamma & Gain [-100..+100].
        // The slot normalizes these to shader-native units (lift / 100 →
        // [-0.5..+0.5], gamma → 2^(v*2/100), gain → 2^(v*2/100)).
        auto *liftLbl  = new QLabel(tr("— Lift —"));
        liftLbl->setAlignment(Qt::AlignCenter);
        hslqLayout->addWidget(liftLbl);
        addHslqRow(tr("R"), -50, 50, 0, QString(), m_hslqLiftR, m_hslqLiftRLabel);
        addHslqRow(tr("G"), -50, 50, 0, QString(), m_hslqLiftG, m_hslqLiftGLabel);
        addHslqRow(tr("B"), -50, 50, 0, QString(), m_hslqLiftB, m_hslqLiftBLabel);

        auto *gammaLbl = new QLabel(tr("— Gamma —"));
        gammaLbl->setAlignment(Qt::AlignCenter);
        hslqLayout->addWidget(gammaLbl);
        addHslqRow(tr("R"), -100, 100, 0, QString(), m_hslqGammaR, m_hslqGammaRLabel);
        addHslqRow(tr("G"), -100, 100, 0, QString(), m_hslqGammaG, m_hslqGammaGLabel);
        addHslqRow(tr("B"), -100, 100, 0, QString(), m_hslqGammaB, m_hslqGammaBLabel);

        auto *gainLbl  = new QLabel(tr("— Gain —"));
        gainLbl->setAlignment(Qt::AlignCenter);
        hslqLayout->addWidget(gainLbl);
        addHslqRow(tr("R"), -100, 100, 0, QString(), m_hslqGainR, m_hslqGainRLabel);
        addHslqRow(tr("G"), -100, 100, 0, QString(), m_hslqGainG, m_hslqGainGLabel);
        addHslqRow(tr("B"), -100, 100, 0, QString(), m_hslqGainB, m_hslqGainBLabel);

        mainLayout->addWidget(hslqGroup);

        // Wire every control to the single onHslQualifierChanged slot.
        QSlider *axisSliders[] = {
            m_hslqHueCenter, m_hslqHueRange, m_hslqSatMin, m_hslqSatMax,
            m_hslqLumaMin, m_hslqLumaMax, m_hslqSoftness,
            m_hslqLiftR, m_hslqLiftG, m_hslqLiftB,
            m_hslqGammaR, m_hslqGammaG, m_hslqGammaB,
            m_hslqGainR, m_hslqGainG, m_hslqGainB
        };
        for (QSlider *s : axisSliders) {
            connect(s, &QSlider::valueChanged,
                    this, &ColorGradingPanel::onHslQualifierChanged);
        }
        connect(m_hslqEnabled, &QCheckBox::toggled,
                this, &ColorGradingPanel::onHslQualifierChanged);

        auto sliderFromDouble =
            [](double value, int minValue, int maxValue) {
                return qBound(minValue,
                              static_cast<int>(std::round(value)),
                              maxValue);
            };
        auto sliderFromFraction =
            [sliderFromDouble](double value) {
                return sliderFromDouble(value * 100.0, 0, 100);
            };
        auto sliderFromLift =
            [sliderFromDouble](double value) {
                return sliderFromDouble(value * 100.0, -50, 50);
            };
        auto sliderFromGammaGain =
            [sliderFromDouble](double value) {
                const double safe = std::max(value, 1e-6);
                return sliderFromDouble(std::log2(safe) * 50.0, -100, 100);
            };

        auto restoreHslSecondaryForClip =
            [this, sliderFromDouble, sliderFromFraction, sliderFromLift,
             sliderFromGammaGain](Timeline *timeline, int trackIdx, int clipIdx) {
                if (trackIdx < 0 || clipIdx < 0) {
                    if (!selectedClipIndex(timeline, &trackIdx, &clipIdx))
                        return;
                }
                const HslSecondaryGrade hsl =
                    clipHslSecondaryAt(timeline, trackIdx, clipIdx);
                if (!m_hslqEnabled || !m_hslqHueCenter || !m_hslqHueRange
                    || !m_hslqSatMin || !m_hslqSatMax || !m_hslqLumaMin
                    || !m_hslqLumaMax || !m_hslqSoftness
                    || !m_hslqLiftR || !m_hslqLiftG || !m_hslqLiftB
                    || !m_hslqGammaR || !m_hslqGammaG || !m_hslqGammaB
                    || !m_hslqGainR || !m_hslqGainG || !m_hslqGainB) {
                    return;
                }

                const bool wasUpdating = m_updating;
                m_updating = true;
                QSignalBlocker be(m_hslqEnabled);
                QSignalBlocker bhc(m_hslqHueCenter);
                QSignalBlocker bhr(m_hslqHueRange);
                QSignalBlocker bsmn(m_hslqSatMin);
                QSignalBlocker bsmx(m_hslqSatMax);
                QSignalBlocker blmn(m_hslqLumaMin);
                QSignalBlocker blmx(m_hslqLumaMax);
                QSignalBlocker bsf(m_hslqSoftness);
                QSignalBlocker blr(m_hslqLiftR);
                QSignalBlocker blg(m_hslqLiftG);
                QSignalBlocker blb(m_hslqLiftB);
                QSignalBlocker bgr(m_hslqGammaR);
                QSignalBlocker bgg(m_hslqGammaG);
                QSignalBlocker bgb(m_hslqGammaB);
                QSignalBlocker bnr(m_hslqGainR);
                QSignalBlocker bng(m_hslqGainG);
                QSignalBlocker bnb(m_hslqGainB);

                const int hueCenter = sliderFromDouble(hsl.hueCenter, 0, 360);
                const int hueRange = sliderFromDouble(hsl.hueRange, 0, 180);
                const int satMin = sliderFromFraction(hsl.satMin);
                const int satMax = sliderFromFraction(hsl.satMax);
                const int lumaMin = sliderFromFraction(hsl.lumaMin);
                const int lumaMax = sliderFromFraction(hsl.lumaMax);
                const int softness = sliderFromDouble(hsl.softness, 0, 50);
                const int liftR = sliderFromLift(hsl.liftR);
                const int liftG = sliderFromLift(hsl.liftG);
                const int liftB = sliderFromLift(hsl.liftB);
                const int gammaR = sliderFromGammaGain(hsl.gammaR);
                const int gammaG = sliderFromGammaGain(hsl.gammaG);
                const int gammaB = sliderFromGammaGain(hsl.gammaB);
                const int gainR = sliderFromGammaGain(hsl.gainR);
                const int gainG = sliderFromGammaGain(hsl.gainG);
                const int gainB = sliderFromGammaGain(hsl.gainB);

                m_hslqEnabled->setChecked(hsl.enabled);
                m_hslqHueCenter->setValue(hueCenter);
                m_hslqHueRange->setValue(hueRange);
                m_hslqSatMin->setValue(satMin);
                m_hslqSatMax->setValue(satMax);
                m_hslqLumaMin->setValue(lumaMin);
                m_hslqLumaMax->setValue(lumaMax);
                m_hslqSoftness->setValue(softness);
                m_hslqLiftR->setValue(liftR);
                m_hslqLiftG->setValue(liftG);
                m_hslqLiftB->setValue(liftB);
                m_hslqGammaR->setValue(gammaR);
                m_hslqGammaG->setValue(gammaG);
                m_hslqGammaB->setValue(gammaB);
                m_hslqGainR->setValue(gainR);
                m_hslqGainG->setValue(gainG);
                m_hslqGainB->setValue(gainB);

                if (m_hslqHueCenterLabel)
                    m_hslqHueCenterLabel->setText(QString::number(hueCenter) + QStringLiteral("°"));
                if (m_hslqHueRangeLabel)
                    m_hslqHueRangeLabel->setText(QString::number(hueRange) + QStringLiteral("°"));
                if (m_hslqSatMinLabel)
                    m_hslqSatMinLabel->setText(QString::number(satMin) + QStringLiteral("%"));
                if (m_hslqSatMaxLabel)
                    m_hslqSatMaxLabel->setText(QString::number(satMax) + QStringLiteral("%"));
                if (m_hslqLumaMinLabel)
                    m_hslqLumaMinLabel->setText(QString::number(lumaMin) + QStringLiteral("%"));
                if (m_hslqLumaMaxLabel)
                    m_hslqLumaMaxLabel->setText(QString::number(lumaMax) + QStringLiteral("%"));
                if (m_hslqSoftnessLabel)
                    m_hslqSoftnessLabel->setText(QString::number(softness));
                if (m_hslqLiftRLabel)  m_hslqLiftRLabel->setText(QString::number(liftR));
                if (m_hslqLiftGLabel)  m_hslqLiftGLabel->setText(QString::number(liftG));
                if (m_hslqLiftBLabel)  m_hslqLiftBLabel->setText(QString::number(liftB));
                if (m_hslqGammaRLabel) m_hslqGammaRLabel->setText(QString::number(gammaR));
                if (m_hslqGammaGLabel) m_hslqGammaGLabel->setText(QString::number(gammaG));
                if (m_hslqGammaBLabel) m_hslqGammaBLabel->setText(QString::number(gammaB));
                if (m_hslqGainRLabel)  m_hslqGainRLabel->setText(QString::number(gainR));
                if (m_hslqGainGLabel)  m_hslqGainGLabel->setText(QString::number(gainG));
                if (m_hslqGainBLabel)  m_hslqGainBLabel->setText(QString::number(gainB));

                m_updating = wasUpdating;
                emit hslQualifierChanged(hsl.enabled,
                                         static_cast<float>(hsl.hueCenter),
                                         static_cast<float>(hsl.hueRange),
                                         static_cast<float>(hsl.satMin),
                                         static_cast<float>(hsl.satMax),
                                         static_cast<float>(hsl.lumaMin),
                                         static_cast<float>(hsl.lumaMax),
                                         static_cast<float>(hsl.softness),
                                         static_cast<float>(hsl.liftR),
                                         static_cast<float>(hsl.liftG),
                                         static_cast<float>(hsl.liftB),
                                         static_cast<float>(hsl.gammaR),
                                         static_cast<float>(hsl.gammaG),
                                         static_cast<float>(hsl.gammaB),
                                         static_cast<float>(hsl.gainR),
                                         static_cast<float>(hsl.gainG),
                                         static_cast<float>(hsl.gainB));
            };

        auto bindTimelineForHslSecondary = [this, restoreHslSecondaryForClip]() {
            Timeline *timeline = findTimelineForColorPanel(this);
            if (!timeline)
                return;
            if (!property("_clipHslSecondarySelectionBound").toBool()) {
                setProperty("_clipHslSecondarySelectionBound", true);
                connect(timeline, &Timeline::clipSelectedOnTrack,
                        this, [this, timeline, restoreHslSecondaryForClip](int trackIdx, int clipIdx) {
                    restoreHslSecondaryForClip(timeline, trackIdx, clipIdx);
                });
            }
            int trackIdx = -1;
            int clipIdx = -1;
            if (selectedClipIndex(timeline, &trackIdx, &clipIdx))
                restoreHslSecondaryForClip(timeline, trackIdx, clipIdx);
        };

        QTimer::singleShot(0, this, bindTimelineForHslSecondary);
        connect(this, &QDockWidget::visibilityChanged,
                this, [bindTimelineForHslSecondary](bool visible) {
            if (visible)
                bindTimelineForHslSecondary();
        });
    }

    // --- US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens
    // Distortion. Sits BELOW HSL クオリファイア because GLPreview applies
    // lens distortion at the very TOP of the fragment shader (a texture
    // coordinate transform BEFORE the texture sample) and blur + sharpen
    // at the very END (POST stage). All sliders default to 0 (identity →
    // free no-op in the shader). ---
    {
        auto *efGroup = new QGroupBox(tr("エフェクト"));
        auto *efLayout = new QVBoxLayout(efGroup);
        efLayout->setSpacing(4);

        auto addEfRow = [&](const QString &name, int min, int max, int initial,
                            QSlider *&outSlider, QLabel *&outLabel) {
            auto *row = new QHBoxLayout;
            auto *lbl = new QLabel(name);
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);
            outSlider = new QSlider(Qt::Horizontal);
            outSlider->setRange(min, max);
            outSlider->setValue(initial);
            row->addWidget(outSlider, 1);
            outLabel = new QLabel(QString::number(initial));
            outLabel->setMinimumWidth(45);
            outLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(outLabel);
            efLayout->addLayout(row);
        };

        addEfRow(tr("Sharpen"),         0, 200, 0, m_efSharpen, m_efSharpenLabel);
        addEfRow(tr("Blur"),            0,  50, 0, m_efBlur,    m_efBlurLabel);
        addEfRow(tr("Lens Distortion"), -100, 100, 0, m_efLens,  m_efLensLabel);

        mainLayout->addWidget(efGroup);

        connect(m_efSharpen, &QSlider::valueChanged,
                this, &ColorGradingPanel::onEffectsPackChanged);
        connect(m_efBlur,    &QSlider::valueChanged,
                this, &ColorGradingPanel::onEffectsPackChanged);
        connect(m_efLens,    &QSlider::valueChanged,
                this, &ColorGradingPanel::onEffectsPackChanged);
    }

    // --- US-3D: 3-axis rotation + perspective foreshortening (Premiere
    // "Basic 3D" / Resolve "Transform" 3D rotation parity). Placed AFTER
    // エフェクト. GLPreview applies an inverse texture-coord warp at the very
    // TOP of the fragment shader (composed AFTER lens distortion). Identity
    // defaults (X=Y=Z=0, perspective=2.0) are a free no-op in the shader. ---
    {
        auto *rotGroup = new QGroupBox(tr("3D 変形"));
        auto *rotLayout = new QVBoxLayout(rotGroup);
        rotLayout->setSpacing(4);

        auto addRotRow = [&](const QString &name, int min, int max, int initial,
                             QSlider *&outSlider, QLabel *&outLabel) {
            auto *row = new QHBoxLayout;
            auto *lbl = new QLabel(name);
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);
            outSlider = new QSlider(Qt::Horizontal);
            outSlider->setRange(min, max);
            outSlider->setValue(initial);
            row->addWidget(outSlider, 1);
            outLabel = new QLabel(QString::number(initial));
            outLabel->setMinimumWidth(45);
            outLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(outLabel);
            rotLayout->addLayout(row);
        };

        addRotRow(tr("X-Rotation"), -180, 180, 0, m_rot3DX, m_rot3DXLabel);
        addRotRow(tr("Y-Rotation"), -180, 180, 0, m_rot3DY, m_rot3DYLabel);
        addRotRow(tr("Z-Rotation"), -180, 180, 0, m_rot3DZ, m_rot3DZLabel);
        // Perspective slider stores 10×distance so we can keep an integer
        // QSlider; 1..100 maps to 0.1..10.0 (default 20 = 2.0).
        addRotRow(tr("Perspective"), 1, 100, 20,
                  m_rot3DPerspective, m_rot3DPerspectiveLabel);
        if (m_rot3DPerspectiveLabel)
            m_rot3DPerspectiveLabel->setText(QStringLiteral("2.0"));

        m_rot3DResetBtn = new QPushButton(tr("リセット"));
        rotLayout->addWidget(m_rot3DResetBtn);

        mainLayout->addWidget(rotGroup);

        connect(m_rot3DX,           &QSlider::valueChanged,
                this, &ColorGradingPanel::onRotation3DChanged);
        connect(m_rot3DY,           &QSlider::valueChanged,
                this, &ColorGradingPanel::onRotation3DChanged);
        connect(m_rot3DZ,           &QSlider::valueChanged,
                this, &ColorGradingPanel::onRotation3DChanged);
        connect(m_rot3DPerspective, &QSlider::valueChanged,
                this, &ColorGradingPanel::onRotation3DChanged);
        connect(m_rot3DResetBtn, &QPushButton::clicked, this, [this]() {
            if (!m_rot3DX || !m_rot3DY || !m_rot3DZ || !m_rot3DPerspective)
                return;
            QSignalBlocker bx(m_rot3DX);
            QSignalBlocker by(m_rot3DY);
            QSignalBlocker bz(m_rot3DZ);
            QSignalBlocker bp(m_rot3DPerspective);
            m_rot3DX->setValue(0);
            m_rot3DY->setValue(0);
            m_rot3DZ->setValue(0);
            m_rot3DPerspective->setValue(20);
            if (m_rot3DXLabel) m_rot3DXLabel->setText(QStringLiteral("0"));
            if (m_rot3DYLabel) m_rot3DYLabel->setText(QStringLiteral("0"));
            if (m_rot3DZLabel) m_rot3DZLabel->setText(QStringLiteral("0"));
            if (m_rot3DPerspectiveLabel)
                m_rot3DPerspectiveLabel->setText(QStringLiteral("2.0"));
            emit rotation3DChanged(0.0f, 0.0f, 0.0f, 2.0f);
        });
    }

    // --- Color Wheels Section ---
    auto *wheelsGroup = new QGroupBox(tr("カラーホイール (Lift / Gamma / Gain)"));
    auto *wheelsLayout = new QHBoxLayout(wheelsGroup);
    wheelsLayout->setSpacing(4);

    m_liftWheel = new ColorWheelWidget(tr("Lift"));
    m_gammaWheel = new ColorWheelWidget(tr("Gamma"));
    m_gainWheel = new ColorWheelWidget(tr("Gain"));

    wheelsLayout->addWidget(m_liftWheel);
    wheelsLayout->addWidget(m_gammaWheel);
    wheelsLayout->addWidget(m_gainWheel);
    mainLayout->addWidget(wheelsGroup);

    connect(m_liftWheel, &ColorWheelWidget::colorChanged,
            this, &ColorGradingPanel::onLiftChanged);
    connect(m_gammaWheel, &ColorWheelWidget::colorChanged,
            this, &ColorGradingPanel::onGammaWheelChanged);
    connect(m_gainWheel, &ColorWheelWidget::colorChanged,
            this, &ColorGradingPanel::onGainChanged);

    // --- Basic Corrections Section ---
    auto *basicGroup = new QGroupBox(tr("基本補正"));
    auto *basicLayout = new QVBoxLayout(basicGroup);
    basicLayout->setSpacing(4);

    m_exposure   = addSlider(basicLayout, tr("露出"),         -300, 300, 0, 100);
    m_brightness = addSlider(basicLayout, tr("明るさ"),       -100, 100, 0);
    m_contrast   = addSlider(basicLayout, tr("コントラスト"), -100, 100, 0);
    m_highlights = addSlider(basicLayout, tr("ハイライト"),   -100, 100, 0);
    m_shadows    = addSlider(basicLayout, tr("シャドウ"),     -100, 100, 0);
    m_saturation = addSlider(basicLayout, tr("彩度"),         -100, 100, 0);
    m_hue        = addSlider(basicLayout, tr("色相"),         -180, 180, 0);
    m_temperature= addSlider(basicLayout, tr("色温度"),       -100, 100, 0);
    m_tint       = addSlider(basicLayout, tr("色かぶり"),     -100, 100, 0);
    m_wbPickButton = new QPushButton(tr("WB スポイト"));
    m_wbPickButton->setCheckable(true);
    m_wbPickButton->setToolTip(tr("プレビューでニュートラルにしたい画素をクリック"));
    basicLayout->addWidget(m_wbPickButton);
    m_gamma      = addSlider(basicLayout, tr("ガンマ"),        10, 300, 100, 100);

    mainLayout->addWidget(basicGroup);
    connect(m_wbPickButton, &QPushButton::toggled,
            this, &ColorGradingPanel::onWhiteBalancePickToggled);

    // --- LUT Section ---
    auto *lutGroup = new QGroupBox(tr("LUT"));
    auto *lutLayout = new QVBoxLayout(lutGroup);

    m_lutCombo = new QComboBox;
    m_lutCombo->addItem(tr("なし"));
    lutLayout->addWidget(m_lutCombo);

    auto *intensityRow = new QHBoxLayout;
    intensityRow->addWidget(new QLabel(tr("強度:")));
    m_lutIntensitySlider = new QSlider(Qt::Horizontal);
    m_lutIntensitySlider->setRange(0, 100);
    m_lutIntensitySlider->setValue(100);
    intensityRow->addWidget(m_lutIntensitySlider);
    m_lutIntensityLabel = new QLabel("100%");
    m_lutIntensityLabel->setMinimumWidth(40);
    intensityRow->addWidget(m_lutIntensityLabel);
    lutLayout->addLayout(intensityRow);

    mainLayout->addWidget(lutGroup);

    connect(m_lutCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ColorGradingPanel::onLutComboChanged);
    connect(m_lutIntensitySlider, &QSlider::valueChanged,
            this, &ColorGradingPanel::onLutIntensityChanged);

    // US-FEAT-C: Lift/Gamma/Gain wheels
    {
        // --- Lift Sliders ---
        auto *liftGroup = new QGroupBox(tr("Lift (シャドウ)"));
        m_liftSliders = addWheelSliders(liftGroup, LiftWheel);
        mainLayout->addWidget(liftGroup);

        // --- Gamma Sliders ---
        auto *gammaGroup = new QGroupBox(tr("Gamma (ミッドトーン)"));
        m_gammaSliders = addWheelSliders(gammaGroup, GammaWheel);
        mainLayout->addWidget(gammaGroup);

        // --- Gain Sliders ---
        auto *gainGroup = new QGroupBox(tr("Gain (ハイライト)"));
        m_gainSliders = addWheelSliders(gainGroup, GainWheel);
        mainLayout->addWidget(gainGroup);
    }

    // --- US-CG-1: RGB Curves Editor ---
    {
        auto *curvesGroup = new QGroupBox(tr("RGB カーブ"));
        auto *curvesLayout = new QVBoxLayout(curvesGroup);
        m_curveEditor = new CurveEditor(this);
        m_curveEditor->setMinimumHeight(280);
        curvesLayout->addWidget(m_curveEditor);
        mainLayout->addWidget(curvesGroup);

        connect(m_curveEditor, &CurveEditor::curvesChanged,
                this, &ColorGradingPanel::curvesChanged);
        connect(m_curveEditor, &CurveEditor::curvesChanged,
                this, [this](const QVector<QVector<int>> &) {
            if (m_updating || !m_curveEditor)
                return;
            writeCurvesToSelectedClip(findTimelineForColorPanel(this),
                                      m_curveEditor->allPoints());
        });

        auto restoreCurveEditorForClip =
            [this](Timeline *timeline, int trackIdx, int clipIdx) {
                if (!m_curveEditor)
                    return;
                if (trackIdx < 0 || clipIdx < 0) {
                    if (!selectedClipIndex(timeline, &trackIdx, &clipIdx))
                        return;
                }
                const ClipCurveData curves = clipCurvesAt(timeline, trackIdx, clipIdx);
                const bool wasUpdating = m_updating;
                m_updating = true;
                m_curveEditor->setAllPoints(curves.editorPointsOrIdentity());
                m_updating = wasUpdating;
            };

        auto bindTimelineForCurves = [this, restoreCurveEditorForClip]() {
            Timeline *timeline = findTimelineForColorPanel(this);
            if (!timeline)
                return;
            if (!property("_clipCurveSelectionBound").toBool()) {
                setProperty("_clipCurveSelectionBound", true);
                connect(timeline, &Timeline::clipSelectedOnTrack,
                        this, [this, timeline, restoreCurveEditorForClip](int trackIdx, int clipIdx) {
                    restoreCurveEditorForClip(timeline, trackIdx, clipIdx);
                });
            }
            int trackIdx = -1;
            int clipIdx = -1;
            if (selectedClipIndex(timeline, &trackIdx, &clipIdx))
                restoreCurveEditorForClip(timeline, trackIdx, clipIdx);
        };

        QTimer::singleShot(0, this, bindTimelineForCurves);
        connect(this, &QDockWidget::visibilityChanged,
                this, [bindTimelineForCurves](bool visible) {
            if (visible)
                bindTimelineForCurves();
        });
    }

    // --- US-CG-4: Hue vs Saturation Curve Editor ---
    {
        auto *hueSatGroup = new QGroupBox(tr("Hue vs Saturation"));
        auto *hueSatLayout = new QVBoxLayout(hueSatGroup);
        m_hueVsSatEditor = new HueVsSatEditor(this);
        m_hueVsSatEditor->setMinimumHeight(180);
        hueSatLayout->addWidget(m_hueVsSatEditor);
        mainLayout->addWidget(hueSatGroup);

        connect(m_hueVsSatEditor, &HueVsSatEditor::hueVsSatChanged,
                this, &ColorGradingPanel::hueVsSatChanged);
    }

    // --- Debounce timer ---
    m_wheelDebounce = new QTimer(this);
    m_wheelDebounce->setSingleShot(true);
    m_wheelDebounce->setInterval(16);
    connect(m_wheelDebounce, &QTimer::timeout,
            this, &ColorGradingPanel::emitWheelsDebounced);

    // --- Reset Button ---
    m_resetButton = new QPushButton(tr("すべてリセット"));
    mainLayout->addWidget(m_resetButton);
    connect(m_resetButton, &QPushButton::clicked,
            this, &ColorGradingPanel::onResetClicked);

    mainLayout->addStretch();

    scrollArea->setWidget(content);
    setWidget(scrollArea);

    // Dark theme styling
    setStyleSheet(R"(
        QGroupBox {
            font-weight: bold;
            border: 1px solid #444;
            border-radius: 4px;
            margin-top: 8px;
            padding-top: 16px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 8px;
            padding: 0 4px;
        }
    )");
}

ColorGradingPanel::SliderRow ColorGradingPanel::addSlider(
    QLayout *layout, const QString &label, int min, int max, int initial, int scale)
{
    auto *row = new QHBoxLayout;
    auto *lbl = new QLabel(label);
    lbl->setMinimumWidth(70);
    row->addWidget(lbl);

    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(min, max);
    slider->setValue(initial);
    row->addWidget(slider, 1);

    auto *valLabel = new QLabel;
    valLabel->setMinimumWidth(45);
    valLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (scale > 1)
        valLabel->setText(QString::number(initial / static_cast<double>(scale), 'f', 2));
    else
        valLabel->setText(QString::number(initial));
    row->addWidget(valLabel);

    layout->addItem(row);

    SliderRow sr;
    sr.slider = slider;
    sr.valueLabel = valLabel;
    sr.paramName = label;

    connect(slider, &QSlider::valueChanged, this, &ColorGradingPanel::onSliderChanged);

    return sr;
}

ColorGradingPanel::WheelSliderGroup ColorGradingPanel::addWheelSliders(QGroupBox *group, ColorGradingPanel::WheelType type)
{
    auto *layout = new QVBoxLayout(group);
    layout->setSpacing(2);

    const bool isGamma = (type == GammaWheel);
    const int sliderMin = isGamma ? 0 : -100;
    const int sliderMax = isGamma ? 100 : 100;
    const int sliderInit = isGamma ? gammaToSlider(1.0) : 0;

    auto makeRow = [&](const QString &label) -> std::pair<QSlider*, QLabel*> {
        auto *row = new QHBoxLayout;
        auto *lbl = new QLabel(label);
        lbl->setMinimumWidth(30);
        row->addWidget(lbl);

        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(sliderMin, sliderMax);
        slider->setValue(sliderInit);
        row->addWidget(slider, 1);

        auto *val = new QLabel;
        val->setMinimumWidth(45);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (isGamma)
            val->setText(QString::number(sliderToGamma(sliderInit), 'f', 2));
        else
            val->setText(QString::number(sliderToLiftGain(sliderInit), 'f', 2));
        row->addWidget(val);

        layout->addLayout(row);
        return {slider, val};
    };

    auto [rSlider, rLabel] = makeRow(tr("R:"));
    auto [gSlider, gLabel] = makeRow(tr("G:"));
    auto [bSlider, bLabel] = makeRow(tr("B:"));
    auto [lSlider, lLabel] = makeRow(tr("Luma:"));

    const QString prefix = (type == LiftWheel)
        ? QStringLiteral("lggLift")
        : (type == GammaWheel ? QStringLiteral("lggGamma") : QStringLiteral("lggGain"));
    rSlider->setObjectName(prefix + QStringLiteral("R"));
    gSlider->setObjectName(prefix + QStringLiteral("G"));
    bSlider->setObjectName(prefix + QStringLiteral("B"));
    lSlider->setObjectName(prefix + QStringLiteral("Luma"));

    connect(rSlider, &QSlider::valueChanged, this, &ColorGradingPanel::onWheelSliderChanged);
    connect(gSlider, &QSlider::valueChanged, this, &ColorGradingPanel::onWheelSliderChanged);
    connect(bSlider, &QSlider::valueChanged, this, &ColorGradingPanel::onWheelSliderChanged);
    connect(lSlider, &QSlider::valueChanged, this, &ColorGradingPanel::onWheelSliderChanged);

    return {rSlider, gSlider, bSlider, lSlider, rLabel, gLabel, bLabel, lLabel};
}

double ColorGradingPanel::sliderToGamma(int v)
{
    if (v <= 50) {
        const double t = std::clamp(v / 50.0, 0.0, 1.0);
        return 0.1 * std::pow(10.0, t);
    }
    const double t = std::clamp((v - 50) / 50.0, 0.0, 1.0);
    return std::pow(4.0, t);
}

int ColorGradingPanel::gammaToSlider(double g)
{
    if (g <= 0.1) return 0;
    if (g >= 4.0) return 100;
    if (g <= 1.0) {
        const double t = std::log(g / 0.1) / std::log(10.0);
        return static_cast<int>(std::round(t * 50.0));
    }
    const double t = std::log(g) / std::log(4.0);
    return 50 + static_cast<int>(std::round(t * 50.0));
}

double ColorGradingPanel::sliderToLiftGain(int v)
{
    return v / 100.0;
}

int ColorGradingPanel::liftGainToSlider(double v)
{
    return static_cast<int>(std::round(v * 100.0));
}

double ColorGradingPanel::wheelGammaToCorrection(double g)
{
    return std::log2(std::max(g, 1e-6));
}

double ColorGradingPanel::correctionGammaToWheel(double g)
{
    return std::pow(2.0, g);
}

ColorWheels ColorGradingPanel::wheelsFromColorCorrection(const ColorCorrection &cc)
{
    ColorWheels cw;
    cw.lift = QVector3D(static_cast<float>(cc.liftR),
                        static_cast<float>(cc.liftG),
                        static_cast<float>(cc.liftB));
    cw.gamma = QVector3D(static_cast<float>(correctionGammaToWheel(cc.gammaR)),
                         static_cast<float>(correctionGammaToWheel(cc.gammaG)),
                         static_cast<float>(correctionGammaToWheel(cc.gammaB)));
    cw.gain = QVector3D(static_cast<float>(cc.gainR),
                        static_cast<float>(cc.gainG),
                        static_cast<float>(cc.gainB));
    cw.liftLuma = 0.0;
    cw.gammaLuma = 1.0;
    cw.gainLuma = 0.0;
    return cw;
}

void ColorGradingPanel::syncColorCorrectionFromWheels(const ColorWheels &cw)
{
    m_cc.liftR = static_cast<double>(cw.lift.x()) + cw.liftLuma;
    m_cc.liftG = static_cast<double>(cw.lift.y()) + cw.liftLuma;
    m_cc.liftB = static_cast<double>(cw.lift.z()) + cw.liftLuma;

    m_cc.gammaR = wheelGammaToCorrection(static_cast<double>(cw.gamma.x()) * cw.gammaLuma);
    m_cc.gammaG = wheelGammaToCorrection(static_cast<double>(cw.gamma.y()) * cw.gammaLuma);
    m_cc.gammaB = wheelGammaToCorrection(static_cast<double>(cw.gamma.z()) * cw.gammaLuma);

    m_cc.gainR = static_cast<double>(cw.gain.x()) + cw.gainLuma;
    m_cc.gainG = static_cast<double>(cw.gain.y()) + cw.gainLuma;
    m_cc.gainB = static_cast<double>(cw.gain.z()) + cw.gainLuma;
}

void ColorGradingPanel::updateBasicTemperatureTintFromCC()
{
    if (!m_temperature.slider || !m_tint.slider)
        return;
    QSignalBlocker bt(m_temperature.slider);
    QSignalBlocker bi(m_tint.slider);
    m_temperature.slider->setValue(static_cast<int>(std::round(m_cc.temperature)));
    m_tint.slider->setValue(static_cast<int>(std::round(m_cc.tint)));
    m_temperature.valueLabel->setText(QString::number(static_cast<int>(m_cc.temperature)));
    m_tint.valueLabel->setText(QString::number(static_cast<int>(m_cc.tint)));
}

void ColorGradingPanel::updateWhiteBalanceControlsFromCC()
{
    if (!m_wbTemperature || !m_wbTint)
        return;
    QSignalBlocker bt(m_wbTemperature);
    QSignalBlocker bi(m_wbTint);
    const int tempSlider = static_cast<int>(std::round(m_cc.temperature));
    const int tintSlider = static_cast<int>(std::round(m_cc.tint));
    m_wbTemperature->setValue(tempSlider);
    m_wbTint->setValue(tintSlider);
    const double K = 5500.0 + static_cast<double>(tempSlider) * 30.0;
    if (m_wbTemperatureLabel)
        m_wbTemperatureLabel->setText(QString::number(static_cast<int>(K)) + "K");
    if (m_wbTintLabel)
        m_wbTintLabel->setText((tintSlider >= 0 ? QStringLiteral("+") : QString())
                               + QString::number(tintSlider));
}

void ColorGradingPanel::updateGraphicalWheelsFromCC()
{
    if (m_liftWheel)
        m_liftWheel->setColor(m_cc.liftR, m_cc.liftG, m_cc.liftB);
    if (m_gammaWheel)
        m_gammaWheel->setColor(m_cc.gammaR, m_cc.gammaG, m_cc.gammaB);
    if (m_gainWheel)
        m_gainWheel->setColor(m_cc.gainR, m_cc.gainG, m_cc.gainB);
}

void ColorGradingPanel::setColorCorrection(const ColorCorrection &cc)
{
    m_cc = cc;
    m_updating = true;
    updateGraphicalWheelsFromCC();
    updateSlidersFromCC();
    updateWhiteBalanceControlsFromCC();
    setWheels(wheelsFromColorCorrection(m_cc));
    m_updating = false;
}

void ColorGradingPanel::applyWhiteBalancePick(const QColor &pixel)
{
    setWhiteBalancePickModeActive(false);
    if (!pixel.isValid())
        return;

    const wbpick::TempTintCorrection correction =
        wbpick::tempTintForNeutral(pixel);
    m_cc.temperature = correction.temperature;
    m_cc.tint = correction.tint;
    updateSlidersFromCC();
    updateWhiteBalanceControlsFromCC();
    emit colorCorrectionChanged(m_cc);
}

void ColorGradingPanel::setWhiteBalancePickModeActive(bool active)
{
    if (!m_wbPickButton)
        return;
    QSignalBlocker blocker(m_wbPickButton);
    m_wbPickButton->setChecked(active);
}

void ColorGradingPanel::updateSlidersFromCC()
{
    blockSliderSignals(true);
    m_exposure.slider->setValue(static_cast<int>(m_cc.exposure * 100));
    m_brightness.slider->setValue(static_cast<int>(m_cc.brightness));
    m_contrast.slider->setValue(static_cast<int>(m_cc.contrast));
    m_highlights.slider->setValue(static_cast<int>(m_cc.highlights));
    m_shadows.slider->setValue(static_cast<int>(m_cc.shadows));
    m_saturation.slider->setValue(static_cast<int>(m_cc.saturation));
    m_hue.slider->setValue(static_cast<int>(m_cc.hue));
    m_temperature.slider->setValue(static_cast<int>(m_cc.temperature));
    m_tint.slider->setValue(static_cast<int>(m_cc.tint));
    m_gamma.slider->setValue(static_cast<int>(m_cc.gamma * 100));

    // Update value labels
    m_exposure.valueLabel->setText(QString::number(m_cc.exposure, 'f', 2));
    m_brightness.valueLabel->setText(QString::number(static_cast<int>(m_cc.brightness)));
    m_contrast.valueLabel->setText(QString::number(static_cast<int>(m_cc.contrast)));
    m_highlights.valueLabel->setText(QString::number(static_cast<int>(m_cc.highlights)));
    m_shadows.valueLabel->setText(QString::number(static_cast<int>(m_cc.shadows)));
    m_saturation.valueLabel->setText(QString::number(static_cast<int>(m_cc.saturation)));
    m_hue.valueLabel->setText(QString::number(static_cast<int>(m_cc.hue)));
    m_temperature.valueLabel->setText(QString::number(static_cast<int>(m_cc.temperature)));
    m_tint.valueLabel->setText(QString::number(static_cast<int>(m_cc.tint)));
    m_gamma.valueLabel->setText(QString::number(m_cc.gamma, 'f', 2));
    blockSliderSignals(false);
}

void ColorGradingPanel::blockSliderSignals(bool block)
{
    m_exposure.slider->blockSignals(block);
    m_brightness.slider->blockSignals(block);
    m_contrast.slider->blockSignals(block);
    m_highlights.slider->blockSignals(block);
    m_shadows.slider->blockSignals(block);
    m_saturation.slider->blockSignals(block);
    m_hue.slider->blockSignals(block);
    m_temperature.slider->blockSignals(block);
    m_tint.slider->blockSignals(block);
    m_gamma.slider->blockSignals(block);
}

void ColorGradingPanel::onLiftChanged(double r, double g, double b)
{
    if (m_updating) return;
    m_cc.liftR = r;
    m_cc.liftG = g;
    m_cc.liftB = b;
    setWheels(wheelsFromColorCorrection(m_cc));
    m_wheelDebounce->start();
}

void ColorGradingPanel::onGammaWheelChanged(double r, double g, double b)
{
    if (m_updating) return;
    m_cc.gammaR = r;
    m_cc.gammaG = g;
    m_cc.gammaB = b;
    setWheels(wheelsFromColorCorrection(m_cc));
    m_wheelDebounce->start();
}

void ColorGradingPanel::onGainChanged(double r, double g, double b)
{
    if (m_updating) return;
    m_cc.gainR = r;
    m_cc.gainG = g;
    m_cc.gainB = b;
    setWheels(wheelsFromColorCorrection(m_cc));
    m_wheelDebounce->start();
}

void ColorGradingPanel::onSliderChanged()
{
    if (m_updating) return;

    m_cc.exposure    = m_exposure.slider->value() / 100.0;
    m_cc.brightness  = m_brightness.slider->value();
    m_cc.contrast    = m_contrast.slider->value();
    m_cc.highlights  = m_highlights.slider->value();
    m_cc.shadows     = m_shadows.slider->value();
    m_cc.saturation  = m_saturation.slider->value();
    m_cc.hue         = m_hue.slider->value();
    m_cc.temperature = m_temperature.slider->value();
    m_cc.tint        = m_tint.slider->value();
    m_cc.gamma       = m_gamma.slider->value() / 100.0;

    // Update labels
    m_exposure.valueLabel->setText(QString::number(m_cc.exposure, 'f', 2));
    m_brightness.valueLabel->setText(QString::number(static_cast<int>(m_cc.brightness)));
    m_contrast.valueLabel->setText(QString::number(static_cast<int>(m_cc.contrast)));
    m_highlights.valueLabel->setText(QString::number(static_cast<int>(m_cc.highlights)));
    m_shadows.valueLabel->setText(QString::number(static_cast<int>(m_cc.shadows)));
    m_saturation.valueLabel->setText(QString::number(static_cast<int>(m_cc.saturation)));
    m_hue.valueLabel->setText(QString::number(static_cast<int>(m_cc.hue)));
    m_temperature.valueLabel->setText(QString::number(static_cast<int>(m_cc.temperature)));
    m_tint.valueLabel->setText(QString::number(static_cast<int>(m_cc.tint)));
    m_gamma.valueLabel->setText(QString::number(m_cc.gamma, 'f', 2));
    updateWhiteBalanceControlsFromCC();

    emit colorCorrectionChanged(m_cc);
}

void ColorGradingPanel::onWheelSliderChanged()
{
    if (m_updating) return;

    auto readLiftGainSliders = [](const WheelSliderGroup &g) -> std::pair<QVector3D, double> {
        return {
            QVector3D(sliderToLiftGain(g.r->value()),
                       sliderToLiftGain(g.g->value()),
                       sliderToLiftGain(g.b->value())),
            sliderToLiftGain(g.luma->value())
        };
    };

    auto readGammaSliders = [](const WheelSliderGroup &g) -> std::pair<QVector3D, double> {
        return {
            QVector3D(static_cast<float>(sliderToGamma(g.r->value())),
                       static_cast<float>(sliderToGamma(g.g->value())),
                       static_cast<float>(sliderToGamma(g.b->value()))),
            sliderToGamma(g.luma->value())
        };
    };

    auto [liftVec, liftLuma] = readLiftGainSliders(m_liftSliders);
    auto [gammaVec, gammaLuma] = readGammaSliders(m_gammaSliders);
    auto [gainVec, gainLuma] = readLiftGainSliders(m_gainSliders);

    m_wheels.lift = liftVec;
    m_wheels.liftLuma = liftLuma;
    m_wheels.gamma = gammaVec;
    m_wheels.gammaLuma = gammaLuma;
    m_wheels.gain = gainVec;
    m_wheels.gainLuma = gainLuma;
    syncColorCorrectionFromWheels(m_wheels);
    updateGraphicalWheelsFromCC();

    // Update labels
    auto fmtLiftGain = [](double v) { return QString::number(v, 'f', 2); };
    auto fmtGamma = [](double v) { return QString::number(v, 'f', 2); };

    m_liftSliders.rLabel->setText(fmtLiftGain(sliderToLiftGain(m_liftSliders.r->value())));
    m_liftSliders.gLabel->setText(fmtLiftGain(sliderToLiftGain(m_liftSliders.g->value())));
    m_liftSliders.bLabel->setText(fmtLiftGain(sliderToLiftGain(m_liftSliders.b->value())));
    m_liftSliders.lumaLabel->setText(fmtLiftGain(liftLuma));

    m_gammaSliders.rLabel->setText(fmtGamma(sliderToGamma(m_gammaSliders.r->value())));
    m_gammaSliders.gLabel->setText(fmtGamma(sliderToGamma(m_gammaSliders.g->value())));
    m_gammaSliders.bLabel->setText(fmtGamma(sliderToGamma(m_gammaSliders.b->value())));
    m_gammaSliders.lumaLabel->setText(fmtGamma(gammaLuma));

    m_gainSliders.rLabel->setText(fmtLiftGain(sliderToLiftGain(m_gainSliders.r->value())));
    m_gainSliders.gLabel->setText(fmtLiftGain(sliderToLiftGain(m_gainSliders.g->value())));
    m_gainSliders.bLabel->setText(fmtLiftGain(sliderToLiftGain(m_gainSliders.b->value())));
    m_gainSliders.lumaLabel->setText(fmtLiftGain(gainLuma));

    m_wheelDebounce->start();
}

void ColorGradingPanel::emitWheelsDebounced()
{
    emit colorWheelsChanged(m_wheels);
}

ColorWheels ColorGradingPanel::currentWheels() const
{
    return m_wheels;
}

void ColorGradingPanel::setWheels(const ColorWheels &cw)
{
    m_wheels = cw;
    syncColorCorrectionFromWheels(cw);

    QSignalBlocker b1(m_liftSliders.r);
    QSignalBlocker b2(m_liftSliders.g);
    QSignalBlocker b3(m_liftSliders.b);
    QSignalBlocker b4(m_liftSliders.luma);
    QSignalBlocker b5(m_gammaSliders.r);
    QSignalBlocker b6(m_gammaSliders.g);
    QSignalBlocker b7(m_gammaSliders.b);
    QSignalBlocker b8(m_gammaSliders.luma);
    QSignalBlocker b9(m_gainSliders.r);
    QSignalBlocker ba(m_gainSliders.g);
    QSignalBlocker bb(m_gainSliders.b);
    QSignalBlocker bc(m_gainSliders.luma);

    m_liftSliders.r->setValue(liftGainToSlider(cw.lift.x()));
    m_liftSliders.g->setValue(liftGainToSlider(cw.lift.y()));
    m_liftSliders.b->setValue(liftGainToSlider(cw.lift.z()));
    m_liftSliders.luma->setValue(liftGainToSlider(cw.liftLuma));

    m_gammaSliders.r->setValue(gammaToSlider(static_cast<double>(cw.gamma.x())));
    m_gammaSliders.g->setValue(gammaToSlider(static_cast<double>(cw.gamma.y())));
    m_gammaSliders.b->setValue(gammaToSlider(static_cast<double>(cw.gamma.z())));
    m_gammaSliders.luma->setValue(gammaToSlider(cw.gammaLuma));

    m_gainSliders.r->setValue(liftGainToSlider(cw.gain.x()));
    m_gainSliders.g->setValue(liftGainToSlider(cw.gain.y()));
    m_gainSliders.b->setValue(liftGainToSlider(cw.gain.z()));
    m_gainSliders.luma->setValue(liftGainToSlider(cw.gainLuma));

    // Update labels
    m_liftSliders.rLabel->setText(QString::number(static_cast<double>(cw.lift.x()), 'f', 2));
    m_liftSliders.gLabel->setText(QString::number(static_cast<double>(cw.lift.y()), 'f', 2));
    m_liftSliders.bLabel->setText(QString::number(static_cast<double>(cw.lift.z()), 'f', 2));
    m_liftSliders.lumaLabel->setText(QString::number(cw.liftLuma, 'f', 2));

    m_gammaSliders.rLabel->setText(QString::number(static_cast<double>(cw.gamma.x()), 'f', 2));
    m_gammaSliders.gLabel->setText(QString::number(static_cast<double>(cw.gamma.y()), 'f', 2));
    m_gammaSliders.bLabel->setText(QString::number(static_cast<double>(cw.gamma.z()), 'f', 2));
    m_gammaSliders.lumaLabel->setText(QString::number(cw.gammaLuma, 'f', 2));

    m_gainSliders.rLabel->setText(QString::number(static_cast<double>(cw.gain.x()), 'f', 2));
    m_gainSliders.gLabel->setText(QString::number(static_cast<double>(cw.gain.y()), 'f', 2));
    m_gainSliders.bLabel->setText(QString::number(static_cast<double>(cw.gain.z()), 'f', 2));
    m_gainSliders.lumaLabel->setText(QString::number(cw.gainLuma, 'f', 2));
    updateGraphicalWheelsFromCC();
}

void ColorGradingPanel::setLutList(const QVector<LutData> &luts)
{
    m_lutCombo->blockSignals(true);
    m_lutCombo->clear();
    m_lutCombo->addItem(tr("なし"));
    for (const auto &lut : luts)
        m_lutCombo->addItem(lut.name);
    m_lutCombo->blockSignals(false);
}

QString ColorGradingPanel::selectedLutName() const
{
    if (m_lutCombo->currentIndex() <= 0)
        return QString();
    return m_lutCombo->currentText();
}

double ColorGradingPanel::lutIntensity() const
{
    return m_lutIntensitySlider->value() / 100.0;
}

QJsonObject ColorGradingPanel::curvesToJson(int srcWidth, int srcHeight) const
{
    ClipCurveData curves;
    if (m_curveEditor)
        curves.setPoints(m_curveEditor->allPoints());
    return curveDataToJson(curves, srcWidth, srcHeight);
}

void ColorGradingPanel::curvesFromJson(const QJsonObject &obj, int, int)
{
    if (!m_curveEditor)
        return;
    const ClipCurveData curves = curveDataFromJson(obj);
    const bool wasUpdating = m_updating;
    m_updating = true;
    m_curveEditor->setAllPoints(curves.editorPointsOrIdentity());
    m_updating = wasUpdating;
}

void ColorGradingPanel::onLutComboChanged(int index)
{
    if (index <= 0)
        emit lutSelected(QString());
    else
        emit lutSelected(m_lutCombo->currentText());
}

void ColorGradingPanel::onLutIntensityChanged(int value)
{
    m_lutIntensityLabel->setText(QString("%1%").arg(value));
    emit lutIntensityChanged(value / 100.0);
}

void ColorGradingPanel::onResetClicked()
{
    m_cc.reset();
    m_updating = true;

    m_liftWheel->setColor(0, 0, 0);
    m_gammaWheel->setColor(0, 0, 0);
    m_gainWheel->setColor(0, 0, 0);
    updateSlidersFromCC();

    // US-FEAT-C: reset wheel sliders to neutral
    ColorWheels neutral;
    setWheels(neutral);

    // US-CG-2: reset WB sliders to identity (5500K / +0).
    if (m_wbTemperature && m_wbTint) {
        QSignalBlocker bt(m_wbTemperature);
        QSignalBlocker bn(m_wbTint);
        m_wbTemperature->setValue(0);
        m_wbTint->setValue(0);
        if (m_wbTemperatureLabel) m_wbTemperatureLabel->setText(tr("5500K"));
        if (m_wbTintLabel) m_wbTintLabel->setText(tr("+0"));
    }
    if (m_wbPickButton && m_wbPickButton->isChecked())
        m_wbPickButton->setChecked(false);

    // US-CG-3: reset vignette sliders to identity (amount=0 → free no-op,
    // midpoint=70 / roundness=0 / feather=30 are the spec defaults).
    if (m_vigAmount && m_vigMidpoint && m_vigRoundness && m_vigFeather) {
        QSignalBlocker ba(m_vigAmount);
        QSignalBlocker bm(m_vigMidpoint);
        QSignalBlocker br(m_vigRoundness);
        QSignalBlocker bf(m_vigFeather);
        m_vigAmount->setValue(0);
        m_vigMidpoint->setValue(70);
        m_vigRoundness->setValue(0);
        m_vigFeather->setValue(30);
        if (m_vigAmountLabel)    m_vigAmountLabel->setText(QStringLiteral("0"));
        if (m_vigMidpointLabel)  m_vigMidpointLabel->setText(QStringLiteral("70"));
        if (m_vigRoundnessLabel) m_vigRoundnessLabel->setText(QStringLiteral("0"));
        if (m_vigFeatherLabel)   m_vigFeatherLabel->setText(QStringLiteral("30"));
    }

    // US-EF-1: reset chroma key to defaults (disabled, green key, 30/60/60/40/10).
    if (m_chromaEnabled && m_chromaKeyColourBtn && m_chromaHueTol
        && m_chromaSatTol && m_chromaLumTol && m_chromaSpill && m_chromaSoftness) {
        QSignalBlocker bce(m_chromaEnabled);
        QSignalBlocker bch(m_chromaHueTol);
        QSignalBlocker bcs(m_chromaSatTol);
        QSignalBlocker bcl(m_chromaLumTol);
        QSignalBlocker bcsp(m_chromaSpill);
        QSignalBlocker bcso(m_chromaSoftness);
        m_chromaEnabled->setChecked(false);
        m_chromaKey = QColor(0, 255, 0);
        m_chromaKeyColourBtn->setStyleSheet(
            QString("background-color: %1;").arg(m_chromaKey.name()));
        m_chromaHueTol->setValue(30);
        m_chromaSatTol->setValue(60);
        m_chromaLumTol->setValue(60);
        m_chromaSpill->setValue(40);
        m_chromaSoftness->setValue(10);
        if (m_chromaHueTolLabel)
            m_chromaHueTolLabel->setText(QStringLiteral("30°"));
        if (m_chromaSatTolLabel)
            m_chromaSatTolLabel->setText(QStringLiteral("60%"));
        if (m_chromaLumTolLabel)
            m_chromaLumTolLabel->setText(QStringLiteral("60%"));
        if (m_chromaSpillLabel)
            m_chromaSpillLabel->setText(QStringLiteral("40"));
        if (m_chromaSoftnessLabel)
            m_chromaSoftnessLabel->setText(QStringLiteral("10"));
    }

    // US-EF-2: reset mask state to defaults (disabled, Rect, no invert,
    // feather=10, rect=center 50%×50%).
    if (m_maskEnabled && m_maskShape && m_maskInvert && m_maskFeather) {
        QSignalBlocker bme(m_maskEnabled);
        QSignalBlocker bms(m_maskShape);
        QSignalBlocker bmi(m_maskInvert);
        QSignalBlocker bmf(m_maskFeather);
        m_maskEnabled->setChecked(false);
        m_maskShape->setCurrentIndex(0);
        m_maskInvert->setChecked(false);
        m_maskFeather->setValue(10);
        m_maskEnabledState   = false;
        m_maskEllipseState   = false;
        m_maskInvertState    = false;
        m_maskFeatherSlider  = 10;
        m_maskCurrentRect    = QRectF(0.25, 0.25, 0.50, 0.50);
        if (m_maskFeatherLabel) m_maskFeatherLabel->setText(QStringLiteral("10"));
        if (m_maskRectXLabel)   m_maskRectXLabel->setText(QStringLiteral("x=0.25"));
        if (m_maskRectYLabel)   m_maskRectYLabel->setText(QStringLiteral("y=0.25"));
        if (m_maskRectWLabel)   m_maskRectWLabel->setText(QStringLiteral("w=0.50"));
        if (m_maskRectHLabel)   m_maskRectHLabel->setText(QStringLiteral("h=0.50"));
    }

    // US-EF-3: reset HSL Qualifier to defaults (disabled, skin-tone hue
    // window 30°/30°, sat 30..100%, luma 30..80%, softness 10, identity
    // lift/gamma/gain). Identity lift/gamma/gain → free no-op even if a
    // glitch re-enables the stage.
    if (m_hslqEnabled && m_hslqHueCenter && m_hslqHueRange
        && m_hslqSatMin && m_hslqSatMax && m_hslqLumaMin && m_hslqLumaMax
        && m_hslqSoftness
        && m_hslqLiftR && m_hslqLiftG && m_hslqLiftB
        && m_hslqGammaR && m_hslqGammaG && m_hslqGammaB
        && m_hslqGainR && m_hslqGainG && m_hslqGainB) {
        QSignalBlocker bhe(m_hslqEnabled);
        QSignalBlocker bhc(m_hslqHueCenter);
        QSignalBlocker bhr(m_hslqHueRange);
        QSignalBlocker bsmin(m_hslqSatMin);
        QSignalBlocker bsmax(m_hslqSatMax);
        QSignalBlocker blmin(m_hslqLumaMin);
        QSignalBlocker blmax(m_hslqLumaMax);
        QSignalBlocker bsoft(m_hslqSoftness);
        QSignalBlocker blr(m_hslqLiftR);   QSignalBlocker blg(m_hslqLiftG);
        QSignalBlocker blb(m_hslqLiftB);
        QSignalBlocker bgr(m_hslqGammaR);  QSignalBlocker bgg(m_hslqGammaG);
        QSignalBlocker bgb(m_hslqGammaB);
        QSignalBlocker bnr(m_hslqGainR);   QSignalBlocker bng(m_hslqGainG);
        QSignalBlocker bnb(m_hslqGainB);

        m_hslqEnabled->setChecked(false);
        m_hslqHueCenter->setValue(30);
        m_hslqHueRange->setValue(30);
        m_hslqSatMin->setValue(30);
        m_hslqSatMax->setValue(100);
        m_hslqLumaMin->setValue(30);
        m_hslqLumaMax->setValue(80);
        m_hslqSoftness->setValue(10);
        m_hslqLiftR->setValue(0);  m_hslqLiftG->setValue(0);  m_hslqLiftB->setValue(0);
        m_hslqGammaR->setValue(0); m_hslqGammaG->setValue(0); m_hslqGammaB->setValue(0);
        m_hslqGainR->setValue(0);  m_hslqGainG->setValue(0);  m_hslqGainB->setValue(0);

        if (m_hslqHueCenterLabel)  m_hslqHueCenterLabel->setText(QStringLiteral("30°"));
        if (m_hslqHueRangeLabel)   m_hslqHueRangeLabel->setText(QStringLiteral("30°"));
        if (m_hslqSatMinLabel)     m_hslqSatMinLabel->setText(QStringLiteral("30%"));
        if (m_hslqSatMaxLabel)     m_hslqSatMaxLabel->setText(QStringLiteral("100%"));
        if (m_hslqLumaMinLabel)    m_hslqLumaMinLabel->setText(QStringLiteral("30%"));
        if (m_hslqLumaMaxLabel)    m_hslqLumaMaxLabel->setText(QStringLiteral("80%"));
        if (m_hslqSoftnessLabel)   m_hslqSoftnessLabel->setText(QStringLiteral("10"));
        if (m_hslqLiftRLabel)  m_hslqLiftRLabel->setText(QStringLiteral("0"));
        if (m_hslqLiftGLabel)  m_hslqLiftGLabel->setText(QStringLiteral("0"));
        if (m_hslqLiftBLabel)  m_hslqLiftBLabel->setText(QStringLiteral("0"));
        if (m_hslqGammaRLabel) m_hslqGammaRLabel->setText(QStringLiteral("0"));
        if (m_hslqGammaGLabel) m_hslqGammaGLabel->setText(QStringLiteral("0"));
        if (m_hslqGammaBLabel) m_hslqGammaBLabel->setText(QStringLiteral("0"));
        if (m_hslqGainRLabel)  m_hslqGainRLabel->setText(QStringLiteral("0"));
        if (m_hslqGainGLabel)  m_hslqGainGLabel->setText(QStringLiteral("0"));
        if (m_hslqGainBLabel)  m_hslqGainBLabel->setText(QStringLiteral("0"));
    }

    if (m_curveEditor)
        m_curveEditor->setAllPoints(ClipCurveData::identityEditorPoints());

    m_updating = false;
    writeCurvesToSelectedClip(findTimelineForColorPanel(this),
                              ClipCurveData::identityEditorPoints());
    writeHslSecondaryToSelectedClip(findTimelineForColorPanel(this),
                                    HslSecondaryGrade{});

    m_lutCombo->setCurrentIndex(0);
    m_lutIntensitySlider->setValue(100);

    emit colorCorrectionChanged(m_cc);
    emit colorWheelsChanged(m_wheels);
    // US-CG-2: emit identity WB so GLPreview clears any pending tint.
    emit whiteBalanceChanged(1.0f, 1.0f, 1.0f);
    // US-CG-3: emit identity vignette so GLPreview clears any pending mask.
    emit vignetteChanged(0.0f, 0.7f, 0.0f, 0.3f);
    // US-EF-1: emit disabled chroma key so GLPreview clears any pending mask.
    // HSL of pure green: H=120/360≈0.333, S=1.0, L=0.5.
    emit chromaKeyChanged(false, 1.0f / 3.0f, 1.0f, 0.5f,
                          30.0f / 180.0f, 0.6f, 0.6f, 0.4f, 0.1f);
    // US-EF-2: emit disabled mask so GLPreview clears the wrap branch.
    emit maskChanged(false, false, false, 0.10f,
                     QRectF(0.25, 0.25, 0.50, 0.50));
    // US-EF-3: emit disabled HSL Qualifier with identity adjustment so
    // GLPreview clears any pending secondary grade.
    emit hslQualifierChanged(false,
                             30.0f, 30.0f,
                             0.30f, 1.00f,
                             0.30f, 0.80f,
                             10.0f,
                             0.0f, 0.0f, 0.0f,
                             1.0f, 1.0f, 1.0f,
                             1.0f, 1.0f, 1.0f);
    // US-EF-4: snap Effects sliders back to identity (Sharpen=Blur=Lens=0)
    // and emit so GLPreview's three uniforms drop to no-op values.
    if (m_efSharpen && m_efBlur && m_efLens) {
        QSignalBlocker bs(m_efSharpen);
        QSignalBlocker bb(m_efBlur);
        QSignalBlocker bl(m_efLens);
        m_efSharpen->setValue(0);
        m_efBlur->setValue(0);
        m_efLens->setValue(0);
        if (m_efSharpenLabel) m_efSharpenLabel->setText(QStringLiteral("0"));
        if (m_efBlurLabel)    m_efBlurLabel->setText(QStringLiteral("0"));
        if (m_efLensLabel)    m_efLensLabel->setText(QStringLiteral("0"));
    }
    emit effectsPackChanged(0.0f, 0.0f, 0.0f);
    // US-3D: snap rotation sliders back to identity (X=Y=Z=0,
    // perspective=20→2.0) and emit so GLPreview drops the warp.
    if (m_rot3DX && m_rot3DY && m_rot3DZ && m_rot3DPerspective) {
        QSignalBlocker bx(m_rot3DX);
        QSignalBlocker by(m_rot3DY);
        QSignalBlocker bz(m_rot3DZ);
        QSignalBlocker bp(m_rot3DPerspective);
        m_rot3DX->setValue(0);
        m_rot3DY->setValue(0);
        m_rot3DZ->setValue(0);
        m_rot3DPerspective->setValue(20);
        if (m_rot3DXLabel) m_rot3DXLabel->setText(QStringLiteral("0"));
        if (m_rot3DYLabel) m_rot3DYLabel->setText(QStringLiteral("0"));
        if (m_rot3DZLabel) m_rot3DZLabel->setText(QStringLiteral("0"));
        if (m_rot3DPerspectiveLabel)
            m_rot3DPerspectiveLabel->setText(QStringLiteral("2.0"));
    }
    emit rotation3DChanged(0.0f, 0.0f, 0.0f, 2.0f);
    emit resetRequested();
}

void ColorGradingPanel::onWhiteBalanceChanged()
{
    if (m_updating || !m_wbTemperature || !m_wbTint)
        return;

    const int tempSlider = m_wbTemperature->value();
    const int tintSlider = m_wbTint->value();
    m_cc.temperature = tempSlider;
    m_cc.tint = tintSlider;
    updateBasicTemperatureTintFromCC();

    // Temperature: slider -100..+100 maps to K = 5500 + slider*30
    // → range 2500..8500 (cool→warm). Higher K = cooler input compensation
    // so we boost red and cut blue when slider > 0 (warm look) and the
    // reverse when slider < 0.
    const double K = 5500.0 + static_cast<double>(tempSlider) * 30.0;
    double rGain = 1.0 + (K - 5500.0) / 3000.0;
    double bGain = 1.0 + (5500.0 - K) / 3000.0;
    rGain = std::clamp(rGain, 0.5, 2.0);
    bGain = std::clamp(bGain, 0.5, 2.0);

    // Tint: slider -100..+100 → t in [-1, 1]. gGain = 1 - t * 0.4.
    const double t = static_cast<double>(tintSlider) / 100.0;
    const double gGain = 1.0 - t * 0.4;

    if (m_wbTemperatureLabel)
        m_wbTemperatureLabel->setText(QString::number(static_cast<int>(K)) + "K");
    if (m_wbTintLabel) {
        QString sign = tintSlider >= 0 ? "+" : "";
        m_wbTintLabel->setText(sign + QString::number(tintSlider));
    }

    emit whiteBalanceChanged(static_cast<float>(rGain),
                             static_cast<float>(gGain),
                             static_cast<float>(bGain));
}

void ColorGradingPanel::onWhiteBalancePickToggled(bool enabled)
{
    emit whiteBalancePickModeRequested(enabled);
}

void ColorGradingPanel::onVignetteChanged()
{
    if (m_updating || !m_vigAmount || !m_vigMidpoint
        || !m_vigRoundness || !m_vigFeather)
        return;

    const int aSlider = m_vigAmount->value();
    const int mSlider = m_vigMidpoint->value();
    const int rSlider = m_vigRoundness->value();
    const int fSlider = m_vigFeather->value();

    // Slider scales: amount [-100..+100] → [-1..+1], midpoint [0..100] →
    // [0..1], roundness [-100..+100] → [-1..+1], feather [0..100] → [0..1].
    const float amount    = aSlider / 100.0f;
    const float midpoint  = mSlider / 100.0f;
    const float roundness = rSlider / 100.0f;
    const float feather   = fSlider / 100.0f;

    if (m_vigAmountLabel)    m_vigAmountLabel->setText(QString::number(aSlider));
    if (m_vigMidpointLabel)  m_vigMidpointLabel->setText(QString::number(mSlider));
    if (m_vigRoundnessLabel) m_vigRoundnessLabel->setText(QString::number(rSlider));
    if (m_vigFeatherLabel)   m_vigFeatherLabel->setText(QString::number(fSlider));

    emit vignetteChanged(amount, midpoint, roundness, feather);
}

// US-EF-1: any chroma-key control changed → recompute HSL of the key colour,
// normalize tolerances/spill/softness to [0..1], emit chromaKeyChanged.
// Hue tolerance is divided by 180 so the shader can compare against the
// circular hue distance which is itself in [0..0.5] after wrap correction.
void ColorGradingPanel::onChromaKeyChanged()
{
    if (m_updating || !m_chromaEnabled || !m_chromaHueTol || !m_chromaSatTol
        || !m_chromaLumTol || !m_chromaSpill || !m_chromaSoftness)
        return;

    const bool enabled = m_chromaEnabled->isChecked();
    const int hueDeg   = m_chromaHueTol->value();
    const int satPct   = m_chromaSatTol->value();
    const int lumPct   = m_chromaLumTol->value();
    const int spillPct = m_chromaSpill->value();
    const int softPct  = m_chromaSoftness->value();

    if (m_chromaHueTolLabel)
        m_chromaHueTolLabel->setText(QString::number(hueDeg) + QStringLiteral("°"));
    if (m_chromaSatTolLabel)
        m_chromaSatTolLabel->setText(QString::number(satPct) + QStringLiteral("%"));
    if (m_chromaLumTolLabel)
        m_chromaLumTolLabel->setText(QString::number(lumPct) + QStringLiteral("%"));
    if (m_chromaSpillLabel)
        m_chromaSpillLabel->setText(QString::number(spillPct));
    if (m_chromaSoftnessLabel)
        m_chromaSoftnessLabel->setText(QString::number(softPct));

    // QColor::getHslF returns h/s/l in [0..1]; hue is -1 for achromatic colours,
    // remap to 0 in that case.
    float h = 0.0f, s = 0.0f, l = 0.0f, a = 1.0f;
    m_chromaKey.getHslF(&h, &s, &l, &a);
    if (h < 0.0f) h = 0.0f;

    const float keyH    = h;
    const float keyS    = s;
    const float keyL    = l;
    const float hueTol  = hueDeg  / 180.0f;
    const float satTol  = satPct  / 100.0f;
    const float lumTol  = lumPct  / 100.0f;
    const float spill   = spillPct / 100.0f;
    const float softness = softPct / 100.0f;

    emit chromaKeyChanged(enabled, keyH, keyS, keyL, hueTol, satTol, lumTol,
                          spill, softness);
}

// US-EF-2: any mask control changed → snapshot state, refresh labels, emit
// maskChanged. The rect itself only changes via setMaskRect (preview drawing
// callback); this slot fires for enable / shape / invert / feather edits.
void ColorGradingPanel::onMaskChanged()
{
    if (m_updating || !m_maskEnabled || !m_maskShape || !m_maskInvert
        || !m_maskFeather)
        return;

    m_maskEnabledState   = m_maskEnabled->isChecked();
    m_maskEllipseState   = (m_maskShape->currentIndex() == 1);
    m_maskInvertState    = m_maskInvert->isChecked();
    m_maskFeatherSlider  = m_maskFeather->value();

    if (m_maskFeatherLabel)
        m_maskFeatherLabel->setText(QString::number(m_maskFeatherSlider));

    // Slider 0..50 → feather 0..0.5 (matches the rect-side smoothstep range
    // expectations in the GLPreview shader; ellipse path treats it the same).
    const float featherNormalized = m_maskFeatherSlider / 100.0f;

    emit maskChanged(m_maskEnabledState, m_maskEllipseState, m_maskInvertState,
                     featherNormalized, m_maskCurrentRect);
}

// US-EF-2: forward the click so MainWindow can call enterMaskEditMode on the
// VideoPlayer. The actual rect arrives back via setMaskRect.
void ColorGradingPanel::onMaskDrawClicked()
{
    emit requestMaskDraw();
}

// US-EF-2: NIT-1 deferred. For this story we just re-emit the current mask
// state as a placeholder; per-clip keyframe storage + linear interpolation
// are deferred to a follow-up so the diff stays scoped to the wiring.
void ColorGradingPanel::onMaskAddKeyframeClicked()
{
    onMaskChanged();
}

// US-EF-3: any HSL Qualifier control changed → snapshot state, refresh
// labels, normalize sat/luma percent → fraction, lift slider/100 →
// [-0.5..+0.5], gamma/gain slider → 2^(v*2/100) for symmetric darken/
// brighten around 1.0, and emit hslQualifierChanged with shader-native
// units. enabled=false → identity payload (gamma/gain=1, lift=0) so the
// shader stage stays a free no-op.
void ColorGradingPanel::onHslQualifierChanged()
{
    if (m_updating || !m_hslqEnabled || !m_hslqHueCenter || !m_hslqHueRange
        || !m_hslqSatMin || !m_hslqSatMax || !m_hslqLumaMin || !m_hslqLumaMax
        || !m_hslqSoftness || !m_hslqLiftR || !m_hslqLiftG || !m_hslqLiftB
        || !m_hslqGammaR || !m_hslqGammaG || !m_hslqGammaB
        || !m_hslqGainR || !m_hslqGainG || !m_hslqGainB)
        return;

    const bool  enabled    = m_hslqEnabled->isChecked();
    const int   hueCenterV = m_hslqHueCenter->value();
    const int   hueRangeV  = m_hslqHueRange->value();
    const int   satMinV    = m_hslqSatMin->value();
    const int   satMaxV    = m_hslqSatMax->value();
    const int   lumaMinV   = m_hslqLumaMin->value();
    const int   lumaMaxV   = m_hslqLumaMax->value();
    const int   softV      = m_hslqSoftness->value();
    const int   liftRV     = m_hslqLiftR->value();
    const int   liftGV     = m_hslqLiftG->value();
    const int   liftBV     = m_hslqLiftB->value();
    const int   gammaRV    = m_hslqGammaR->value();
    const int   gammaGV    = m_hslqGammaG->value();
    const int   gammaBV    = m_hslqGammaB->value();
    const int   gainRV     = m_hslqGainR->value();
    const int   gainGV     = m_hslqGainG->value();
    const int   gainBV     = m_hslqGainB->value();

    if (m_hslqHueCenterLabel)
        m_hslqHueCenterLabel->setText(QString::number(hueCenterV) + QStringLiteral("°"));
    if (m_hslqHueRangeLabel)
        m_hslqHueRangeLabel->setText(QString::number(hueRangeV) + QStringLiteral("°"));
    if (m_hslqSatMinLabel)
        m_hslqSatMinLabel->setText(QString::number(satMinV) + QStringLiteral("%"));
    if (m_hslqSatMaxLabel)
        m_hslqSatMaxLabel->setText(QString::number(satMaxV) + QStringLiteral("%"));
    if (m_hslqLumaMinLabel)
        m_hslqLumaMinLabel->setText(QString::number(lumaMinV) + QStringLiteral("%"));
    if (m_hslqLumaMaxLabel)
        m_hslqLumaMaxLabel->setText(QString::number(lumaMaxV) + QStringLiteral("%"));
    if (m_hslqSoftnessLabel)
        m_hslqSoftnessLabel->setText(QString::number(softV));
    if (m_hslqLiftRLabel)  m_hslqLiftRLabel->setText(QString::number(liftRV));
    if (m_hslqLiftGLabel)  m_hslqLiftGLabel->setText(QString::number(liftGV));
    if (m_hslqLiftBLabel)  m_hslqLiftBLabel->setText(QString::number(liftBV));
    if (m_hslqGammaRLabel) m_hslqGammaRLabel->setText(QString::number(gammaRV));
    if (m_hslqGammaGLabel) m_hslqGammaGLabel->setText(QString::number(gammaGV));
    if (m_hslqGammaBLabel) m_hslqGammaBLabel->setText(QString::number(gammaBV));
    if (m_hslqGainRLabel)  m_hslqGainRLabel->setText(QString::number(gainRV));
    if (m_hslqGainGLabel)  m_hslqGainGLabel->setText(QString::number(gainGV));
    if (m_hslqGainBLabel)  m_hslqGainBLabel->setText(QString::number(gainBV));

    // Shader-native conversions:
    //   lift  : v / 100 → [-0.5, +0.5] additive
    //   gamma : 2^(v / 50)  → [0.25, 4.0] (identity 1.0 at v=0)
    //   gain  : 2^(v / 50)  → [0.25, 4.0] (identity 1.0 at v=0)
    const float hueCenter = static_cast<float>(hueCenterV);
    const float hueRange  = static_cast<float>(hueRangeV);
    const float satMin    = satMinV  / 100.0f;
    const float satMax    = satMaxV  / 100.0f;
    const float lumaMin   = lumaMinV / 100.0f;
    const float lumaMax   = lumaMaxV / 100.0f;
    const float softness  = static_cast<float>(softV);
    const float liftR     = liftRV / 100.0f;
    const float liftG     = liftGV / 100.0f;
    const float liftB     = liftBV / 100.0f;
    const float gammaR    = std::pow(2.0f, gammaRV / 50.0f);
    const float gammaG    = std::pow(2.0f, gammaGV / 50.0f);
    const float gammaB    = std::pow(2.0f, gammaBV / 50.0f);
    const float gainR     = std::pow(2.0f, gainRV  / 50.0f);
    const float gainG     = std::pow(2.0f, gainGV  / 50.0f);
    const float gainB     = std::pow(2.0f, gainBV  / 50.0f);

    HslSecondaryGrade hsl;
    hsl.enabled = enabled;
    hsl.hueCenter = hueCenter;
    hsl.hueRange = hueRange;
    hsl.satMin = satMin;
    hsl.satMax = satMax;
    hsl.lumaMin = lumaMin;
    hsl.lumaMax = lumaMax;
    hsl.softness = softness;
    hsl.liftR = liftR;
    hsl.liftG = liftG;
    hsl.liftB = liftB;
    hsl.gammaR = gammaR;
    hsl.gammaG = gammaG;
    hsl.gammaB = gammaB;
    hsl.gainR = gainR;
    hsl.gainG = gainG;
    hsl.gainB = gainB;
    writeHslSecondaryToSelectedClip(findTimelineForColorPanel(this), hsl);

    emit hslQualifierChanged(hsl.enabled,
                             static_cast<float>(hsl.hueCenter),
                             static_cast<float>(hsl.hueRange),
                             static_cast<float>(hsl.satMin),
                             static_cast<float>(hsl.satMax),
                             static_cast<float>(hsl.lumaMin),
                             static_cast<float>(hsl.lumaMax),
                             static_cast<float>(hsl.softness),
                             static_cast<float>(hsl.liftR),
                             static_cast<float>(hsl.liftG),
                             static_cast<float>(hsl.liftB),
                             static_cast<float>(hsl.gammaR),
                             static_cast<float>(hsl.gammaG),
                             static_cast<float>(hsl.gammaB),
                             static_cast<float>(hsl.gainR),
                             static_cast<float>(hsl.gainG),
                             static_cast<float>(hsl.gainB));
}

// US-EF-4: any Effects (Sharpen / Blur / Lens Distortion) slider changed.
// Refresh the value labels and emit effectsPackChanged with the raw slider
// scalars; GLPreview applies the per-stage multipliers (×0.01 / px / ×0.01)
// inside the fragment shader. All zeros = identity (free no-op).
void ColorGradingPanel::onEffectsPackChanged()
{
    if (m_updating || !m_efSharpen || !m_efBlur || !m_efLens)
        return;

    const int sharpen = m_efSharpen->value();
    const int blur    = m_efBlur->value();
    const int lens    = m_efLens->value();

    if (m_efSharpenLabel) m_efSharpenLabel->setText(QString::number(sharpen));
    if (m_efBlurLabel)    m_efBlurLabel->setText(QString::number(blur));
    if (m_efLensLabel) {
        const QString sign = lens > 0 ? QStringLiteral("+") : QString();
        m_efLensLabel->setText(sign + QString::number(lens));
    }

    emit effectsPackChanged(static_cast<float>(sharpen),
                            static_cast<float>(blur),
                            static_cast<float>(lens));
}

// US-3D: 3-axis rotation + perspective foreshortening slider changed.
// Refresh value labels and emit rotation3DChanged with the converted
// values; the perspective slider stores 10×distance so divide back to
// the float [0.1..10.0] domain expected by GLPreview::setRotation3D.
void ColorGradingPanel::onRotation3DChanged()
{
    if (m_updating || !m_rot3DX || !m_rot3DY || !m_rot3DZ
        || !m_rot3DPerspective)
        return;

    const int xDeg = m_rot3DX->value();
    const int yDeg = m_rot3DY->value();
    const int zDeg = m_rot3DZ->value();
    const int persRaw = m_rot3DPerspective->value();
    const float persDist = static_cast<float>(persRaw) / 10.0f;

    auto signedLabel = [](int v) {
        return (v > 0 ? QStringLiteral("+") : QString()) + QString::number(v);
    };
    if (m_rot3DXLabel) m_rot3DXLabel->setText(signedLabel(xDeg));
    if (m_rot3DYLabel) m_rot3DYLabel->setText(signedLabel(yDeg));
    if (m_rot3DZLabel) m_rot3DZLabel->setText(signedLabel(zDeg));
    if (m_rot3DPerspectiveLabel)
        m_rot3DPerspectiveLabel->setText(QString::number(persDist, 'f', 1));

    emit rotation3DChanged(static_cast<float>(xDeg),
                           static_cast<float>(yDeg),
                           static_cast<float>(zDeg),
                           persDist);
}

// US-EF-2: receive the normalized QRectF from VideoPlayer's mask-edit
// callback, refresh labels, re-emit maskChanged so GLPreview picks up the
// new geometry. Clamps to [0..1] in case the picker returned a rect that
// touched the letterbox edges.
void ColorGradingPanel::setMaskRect(const QRectF &normalizedRect)
{
    QRectF r = normalizedRect;
    if (!r.isValid() || r.width() <= 0.0 || r.height() <= 0.0)
        return;

    qreal x = std::clamp<qreal>(r.x(), 0.0, 1.0);
    qreal y = std::clamp<qreal>(r.y(), 0.0, 1.0);
    qreal w = std::clamp<qreal>(r.width(),  0.001, 1.0 - x);
    qreal h = std::clamp<qreal>(r.height(), 0.001, 1.0 - y);
    m_maskCurrentRect = QRectF(x, y, w, h);

    if (m_maskRectXLabel)
        m_maskRectXLabel->setText(QString("x=%1").arg(x, 0, 'f', 2));
    if (m_maskRectYLabel)
        m_maskRectYLabel->setText(QString("y=%1").arg(y, 0, 'f', 2));
    if (m_maskRectWLabel)
        m_maskRectWLabel->setText(QString("w=%1").arg(w, 0, 'f', 2));
    if (m_maskRectHLabel)
        m_maskRectHLabel->setText(QString("h=%1").arg(h, 0, 'f', 2));

    const float featherNormalized = m_maskFeatherSlider / 100.0f;
    emit maskChanged(m_maskEnabledState, m_maskEllipseState, m_maskInvertState,
                     featherNormalized, m_maskCurrentRect);
}

// US-EF-1: open QColorDialog seeded with the current key colour. On accept,
// refresh the swatch and re-emit chromaKeyChanged with the new HSL value.
void ColorGradingPanel::onChromaKeyColourClicked()
{
    QColor picked = QColorDialog::getColor(m_chromaKey, this,
                                            tr("クロマキー色を選択"));
    if (!picked.isValid())
        return;
    m_chromaKey = picked;
    if (m_chromaKeyColourBtn)
        m_chromaKeyColourBtn->setStyleSheet(
            QString("background-color: %1;").arg(m_chromaKey.name()));
    onChromaKeyChanged();
}
