#include "EffectControlsPanel.h"
#include "EffectControlsToolbar.h"
#include "EffectKeyframeToggle.h"
#include "EffectRowWidget.h"
#include "EffectParamSchema.h"
#include "Keyframe.h"
#include "MainWindow.h"
#include "Timeline.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QMessageBox>
#include <algorithm>
#include <cmath>

namespace effectctrl {

namespace {

const QStringList &motionTrackNames()
{
    static const QStringList names = {
        QStringLiteral("motion.position.x"),
        QStringLiteral("motion.position.y"),
        QStringLiteral("motion.scale"),
        QStringLiteral("motion.rotation"),
        QStringLiteral("motion.opacity"),
        QStringLiteral("motion.position.z"),
        QStringLiteral("motion.rotation.x"),
        QStringLiteral("motion.rotation.y"),
        QStringLiteral("motion.rotation.z"),
    };
    return names;
}

struct EffectTrackRef {
    QString trackName;
    QString paramName;
    int effectIndex = -1;
};

bool parseEffectTrackName(const QString &trackName, int *effectIndex, QString *paramName)
{
    static const QString prefix = QStringLiteral("effect.");
    if (!trackName.startsWith(prefix)) {
        return false;
    }

    const int indexStart = prefix.size();
    const int dotPos = trackName.indexOf(QLatin1Char('.'), indexStart);
    if (dotPos <= indexStart) {
        return false;
    }

    bool ok = false;
    const int parsedIndex = trackName.mid(indexStart, dotPos - indexStart).toInt(&ok);
    if (!ok || parsedIndex < 0) {
        return false;
    }

    if (effectIndex) {
        *effectIndex = parsedIndex;
    }
    if (paramName) {
        *paramName = trackName.mid(dotPos + 1);
    }
    return true;
}

QString effectTrackName(int effectIndex, const QString &paramName)
{
    return QStringLiteral("effect.%1.%2").arg(effectIndex).arg(paramName);
}

const QStringList &colorChannelNames()
{
    static const QStringList names = {
        QStringLiteral("r"),
        QStringLiteral("g"),
        QStringLiteral("b"),
    };
    return names;
}

QString effectColorChannelTrackName(int effectIndex,
                                    const QString &paramName,
                                    const QString &channel)
{
    return QStringLiteral("effect.%1.%2.%3")
        .arg(effectIndex)
        .arg(paramName)
        .arg(channel);
}

bool isColorParam(VideoEffectType type, const QString &paramName)
{
    const auto schema = paramSchemaFor(type);
    for (const auto &def : schema) {
        if (def.name == paramName)
            return def.type == ParamType::Color;
    }
    return false;
}

bool hasColorChannelTrack(const KeyframeManager &keyframes,
                          int effectIndex,
                          const QString &paramName)
{
    for (const QString &channel : colorChannelNames()) {
        if (keyframes.hasTrack(effectColorChannelTrackName(effectIndex, paramName, channel)))
            return true;
    }
    return false;
}

KeyframeTrack *firstColorChannelTrack(KeyframeManager &keyframes,
                                      int effectIndex,
                                      const QString &paramName)
{
    for (const QString &channel : colorChannelNames()) {
        if (auto *track = keyframes.track(effectColorChannelTrackName(effectIndex, paramName, channel)))
            return track;
    }
    return nullptr;
}

double colorChannelValue(const QColor &color, const QString &channel)
{
    if (channel == QStringLiteral("r"))
        return color.red();
    if (channel == QStringLiteral("g"))
        return color.green();
    if (channel == QStringLiteral("b"))
        return color.blue();
    return 0.0;
}

QVector<EffectTrackRef> effectKeyframeTracksAt(const KeyframeManager &keyframes, int effectIndex)
{
    QVector<EffectTrackRef> refs;
    for (const auto &track : keyframes.tracks()) {
        int parsedIndex = -1;
        QString paramName;
        if (parseEffectTrackName(track.propertyName(), &parsedIndex, &paramName)
            && parsedIndex == effectIndex) {
            refs.append(EffectTrackRef{track.propertyName(), paramName, parsedIndex});
        }
    }
    return refs;
}

QVector<EffectTrackRef> effectKeyframeTracksFrom(const KeyframeManager &keyframes, int firstEffectIndex)
{
    QVector<EffectTrackRef> refs;
    for (const auto &track : keyframes.tracks()) {
        int parsedIndex = -1;
        QString paramName;
        if (parseEffectTrackName(track.propertyName(), &parsedIndex, &paramName)
            && parsedIndex >= firstEffectIndex) {
            refs.append(EffectTrackRef{track.propertyName(), paramName, parsedIndex});
        }
    }
    return refs;
}

QString uniqueTemporaryTrackName(const KeyframeManager &keyframes,
                                 int effectIndex,
                                 const QString &paramName)
{
    QString candidate = QStringLiteral("__effect_keyframe_remap_tmp.%1.%2")
                            .arg(effectIndex)
                            .arg(paramName);
    while (keyframes.hasTrack(candidate)) {
        candidate.append(QLatin1Char('_'));
    }
    return candidate;
}

bool copyKeyframeTrack(KeyframeManager &keyframes,
                       const QString &sourceTrackName,
                       const QString &targetTrackName)
{
    const KeyframeTrack *source = keyframes.track(sourceTrackName);
    if (!source) {
        return false;
    }

    KeyframeTrack copy = *source;
    copy.setPropertyName(targetTrackName);
    keyframes.addTrack(copy);
    keyframes.setLoopOutMode(targetTrackName, keyframes.loopOutMode(sourceTrackName));
    return true;
}

void shiftEffectKeyframeTracks(KeyframeManager &keyframes, int firstEffectIndex, int delta)
{
    auto refs = effectKeyframeTracksFrom(keyframes, firstEffectIndex);
    std::sort(refs.begin(), refs.end(),
              [delta](const EffectTrackRef &a, const EffectTrackRef &b) {
                  if (a.effectIndex == b.effectIndex) {
                      return a.paramName < b.paramName;
                  }
                  return delta > 0
                      ? a.effectIndex > b.effectIndex
                      : a.effectIndex < b.effectIndex;
              });

    for (const auto &ref : refs) {
        keyframes.renameTrack(ref.trackName,
                              effectTrackName(ref.effectIndex + delta, ref.paramName));
    }
}

void swapEffectKeyframeTracks(KeyframeManager &keyframes, int firstEffectIndex, int secondEffectIndex)
{
    if (firstEffectIndex == secondEffectIndex) {
        return;
    }
    if (firstEffectIndex > secondEffectIndex) {
        std::swap(firstEffectIndex, secondEffectIndex);
    }

    const auto firstRefs = effectKeyframeTracksAt(keyframes, firstEffectIndex);
    const auto secondRefs = effectKeyframeTracksAt(keyframes, secondEffectIndex);
    QVector<EffectTrackRef> temporaryRefs;

    for (const auto &ref : firstRefs) {
        const QString tempName = uniqueTemporaryTrackName(keyframes, ref.effectIndex, ref.paramName);
        if (keyframes.renameTrack(ref.trackName, tempName)) {
            temporaryRefs.append(EffectTrackRef{tempName, ref.paramName, ref.effectIndex});
        }
    }
    for (const auto &ref : secondRefs) {
        keyframes.renameTrack(ref.trackName, effectTrackName(firstEffectIndex, ref.paramName));
    }
    for (const auto &ref : temporaryRefs) {
        keyframes.renameTrack(ref.trackName, effectTrackName(secondEffectIndex, ref.paramName));
    }
}

void removeEffectKeyframeTracks(KeyframeManager &keyframes, int removedEffectIndex)
{
    const auto removedRefs = effectKeyframeTracksAt(keyframes, removedEffectIndex);
    for (const auto &ref : removedRefs) {
        keyframes.removeTrack(ref.trackName);
    }
    shiftEffectKeyframeTracks(keyframes, removedEffectIndex + 1, -1);
}

void duplicateEffectKeyframeTracks(KeyframeManager &keyframes, int sourceEffectIndex)
{
    const auto sourceRefs = effectKeyframeTracksAt(keyframes, sourceEffectIndex);
    const int insertedEffectIndex = sourceEffectIndex + 1;
    shiftEffectKeyframeTracks(keyframes, insertedEffectIndex, 1);
    for (const auto &ref : sourceRefs) {
        copyKeyframeTrack(keyframes, ref.trackName,
                          effectTrackName(insertedEffectIndex, ref.paramName));
    }
}

ClipInfo *selectedClipForKey(const Timeline *timeline, const ClipKey &key)
{
    if (!timeline || !key.valid()) {
        return nullptr;
    }

    const auto &videoTracks = timeline->videoTracks();
    if (key.trackIdx < 0 || key.trackIdx >= videoTracks.size() || !videoTracks[key.trackIdx]) {
        return nullptr;
    }

    const auto &clips = videoTracks[key.trackIdx]->clips();
    if (key.clipIdx < 0 || key.clipIdx >= clips.size()) {
        return nullptr;
    }

    return &const_cast<ClipInfo &>(clips[key.clipIdx]);
}

double clipTimelineStartSeconds(const Timeline *timeline, const ClipKey &key)
{
    if (!timeline || !key.valid()) {
        return 0.0;
    }

    const auto &videoTracks = timeline->videoTracks();
    if (key.trackIdx < 0 || key.trackIdx >= videoTracks.size() || !videoTracks[key.trackIdx]) {
        return 0.0;
    }

    const auto &clips = videoTracks[key.trackIdx]->clips();
    if (key.clipIdx < 0 || key.clipIdx >= clips.size()) {
        return 0.0;
    }

    double cursor = 0.0;
    for (int i = 0; i < key.clipIdx; ++i) {
        const double clipStart = cursor + clips[i].leadInSec;
        cursor = clipStart + clips[i].effectiveDuration();
    }
    return cursor + clips[key.clipIdx].leadInSec;
}

constexpr double boundedClipLocalSeconds(double playheadSeconds,
                                         double clipStartSeconds,
                                         double clipDurationSeconds)
{
    if (clipDurationSeconds <= 0.0 || playheadSeconds <= clipStartSeconds) {
        return 0.0;
    }

    const double localSeconds = playheadSeconds - clipStartSeconds;
    return localSeconds >= clipDurationSeconds ? clipDurationSeconds : localSeconds;
}

static_assert(boundedClipLocalSeconds(7.0, 5.0, 10.0) == 2.0,
              "initial keyframe time must be clip-local seconds");

} // namespace

EffectControlsPanel::EffectControlsPanel(QWidget *parent)
    : QDockWidget("Effect Controls", parent)
{
    setObjectName("EffectControlsDock");

    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(0);

    m_panelToolbar = new EffectControlsPanelToolbar(centralWidget);
    connect(m_panelToolbar, &EffectControlsPanelToolbar::addEffectRequested,
            this, &EffectControlsPanel::onAddEffectRequested);
    mainLayout->addWidget(m_panelToolbar);

    m_scrollArea = new QScrollArea(centralWidget);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mainLayout->addWidget(m_scrollArea);

    m_scrollContent = new QWidget();
    m_scrollLayout = new QVBoxLayout(m_scrollContent);
    m_scrollLayout->setContentsMargins(2, 2, 2, 2);
    m_scrollLayout->setSpacing(4);
    m_motionWidget = new MotionSectionWidget(m_scrollContent);
    m_motionWidget->setEnabled(false);
    m_scrollLayout->addWidget(m_motionWidget);
    buildLayerStyleGroup();
    m_scrollLayout->addStretch();
    m_scrollArea->setWidget(m_scrollContent);

    connect(m_motionWidget, &MotionSectionWidget::motionChanged,
            this, [this](const MotionState &motion) {
                if (m_timeline && m_currentClipKey.valid()) {
                    m_timeline->setClipMotion(m_currentClipKey.trackIdx,
                                              m_currentClipKey.clipIdx,
                                              motion);
                }
                emit motionChanged(motion);
            });
    connect(m_motionWidget, &MotionSectionWidget::keyframeToggled,
            this, &EffectControlsPanel::onMotionKeyframeToggled);
    connect(m_motionWidget, &MotionSectionWidget::keyframeTrackChanged,
            this, [this](const QString &) { persistEditedKeyframes(); });

    buildEmptyState();

    setWidget(centralWidget);
    setMinimumWidth(280);
}

void EffectControlsPanel::buildLayerStyleGroup()
{
    m_layerStyleGroup = new QGroupBox(QStringLiteral("レイヤースタイル"), m_scrollContent);
    auto *form = new QFormLayout(m_layerStyleGroup);
    form->setContentsMargins(4, 4, 4, 4);
    form->setSpacing(4);

    m_shadowEnable = new QCheckBox(QStringLiteral("ドロップシャドウ"), m_layerStyleGroup);
    form->addRow(m_shadowEnable);

    m_shadowColorButton = new QPushButton(QStringLiteral("色"), m_layerStyleGroup);
    m_shadowColorButton->setMinimumWidth(64);
    form->addRow(QStringLiteral("影の色"), m_shadowColorButton);

    auto *offsetRow = new QHBoxLayout();
    m_shadowOffsetX = new QDoubleSpinBox(m_layerStyleGroup);
    m_shadowOffsetX->setRange(-500.0, 500.0);
    m_shadowOffsetX->setDecimals(1);
    m_shadowOffsetX->setSingleStep(1.0);
    m_shadowOffsetY = new QDoubleSpinBox(m_layerStyleGroup);
    m_shadowOffsetY->setRange(-500.0, 500.0);
    m_shadowOffsetY->setDecimals(1);
    m_shadowOffsetY->setSingleStep(1.0);
    offsetRow->addWidget(m_shadowOffsetX);
    offsetRow->addWidget(m_shadowOffsetY);
    form->addRow(QStringLiteral("オフセット X/Y"), offsetRow);

    m_shadowBlurRadius = new QDoubleSpinBox(m_layerStyleGroup);
    m_shadowBlurRadius->setRange(0.0, 200.0);
    m_shadowBlurRadius->setDecimals(1);
    m_shadowBlurRadius->setSingleStep(1.0);
    form->addRow(QStringLiteral("ぼかし半径"), m_shadowBlurRadius);

    auto *opacityRow = new QHBoxLayout();
    m_shadowOpacitySlider = new QSlider(Qt::Horizontal, m_layerStyleGroup);
    m_shadowOpacitySlider->setRange(0, 100);
    m_shadowOpacitySpin = new QSpinBox(m_layerStyleGroup);
    m_shadowOpacitySpin->setRange(0, 100);
    m_shadowOpacitySpin->setSuffix(QStringLiteral("%"));
    opacityRow->addWidget(m_shadowOpacitySlider, 1);
    opacityRow->addWidget(m_shadowOpacitySpin);
    form->addRow(QStringLiteral("不透明度"), opacityRow);

    m_strokeEnable = new QCheckBox(QStringLiteral("ストローク(縁取り)"), m_layerStyleGroup);
    form->addRow(m_strokeEnable);

    m_strokeColorButton = new QPushButton(QStringLiteral("色"), m_layerStyleGroup);
    m_strokeColorButton->setMinimumWidth(64);
    form->addRow(QStringLiteral("縁取り色"), m_strokeColorButton);

    m_strokeWidth = new QDoubleSpinBox(m_layerStyleGroup);
    m_strokeWidth->setRange(0.0, 200.0);
    m_strokeWidth->setDecimals(1);
    m_strokeWidth->setSingleStep(1.0);
    form->addRow(QStringLiteral("幅"), m_strokeWidth);

    connect(m_shadowEnable, &QCheckBox::toggled, this, [this](bool) {
        if (m_blockLayerStyleUi)
            return;
        updateLayerStyleControlAvailability(m_currentClipKey.valid());
        persistLayerStyleFromControls();
    });
    connect(m_shadowColorButton, &QPushButton::clicked,
            this, &EffectControlsPanel::chooseShadowColor);
    connect(m_shadowOffsetX, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double) { persistLayerStyleFromControls(); });
    connect(m_shadowOffsetY, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double) { persistLayerStyleFromControls(); });
    connect(m_shadowBlurRadius, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double) { persistLayerStyleFromControls(); });
    connect(m_shadowOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_blockLayerStyleUi)
            return;
        QSignalBlocker blocker(m_shadowOpacitySpin);
        m_shadowOpacitySpin->setValue(value);
        persistLayerStyleFromControls();
    });
    connect(m_shadowOpacitySpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
        if (m_blockLayerStyleUi)
            return;
        QSignalBlocker blocker(m_shadowOpacitySlider);
        m_shadowOpacitySlider->setValue(value);
        persistLayerStyleFromControls();
    });
    connect(m_strokeEnable, &QCheckBox::toggled, this, [this](bool) {
        if (m_blockLayerStyleUi)
            return;
        updateLayerStyleControlAvailability(m_currentClipKey.valid());
        persistLayerStyleFromControls();
    });
    connect(m_strokeColorButton, &QPushButton::clicked,
            this, &EffectControlsPanel::chooseStrokeColor);
    connect(m_strokeWidth, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double) { persistLayerStyleFromControls(); });

    m_scrollLayout->addWidget(m_layerStyleGroup);
    setLayerStyleControls(LayerStyle{}, false);
}

void EffectControlsPanel::setTimeline(Timeline *timeline)
{
    m_timeline = timeline;
    if (!m_timeline)
        return;

    connect(m_timeline, &Timeline::clipSelectedOnTrack, this,
            [this](int trackIdx, int clipIdx) {
                m_currentClipKey = {trackIdx, clipIdx};
                refreshFromCurrentClip();
            });
}

void EffectControlsPanel::setMainWindow(MainWindow *mw)
{
    m_mainWindow = mw;
    if (!m_mainWindow) {
        return;
    }

    connect(m_mainWindow, &MainWindow::playheadSecondsChanged, this,
            [this](double) { setPlayheadOnVisibleNavBars(currentClipLocalPlayheadSeconds()); });
    setPlayheadOnVisibleNavBars(currentClipLocalPlayheadSeconds());
}

void EffectControlsPanel::refreshFromCurrentClip()
{
    if (!m_timeline) {
        buildEmptyState();
        return;
    }

    const int trackIdx = m_currentClipKey.trackIdx;
    const int clipIdx = m_currentClipKey.clipIdx;

    if (!m_currentClipKey.valid()) {
        buildEmptyState();
        return;
    }

    const auto &vTracks = m_timeline->videoTracks();
    if (trackIdx < 0 || trackIdx >= vTracks.size()) {
        buildEmptyState();
        return;
    }
    if (!vTracks[trackIdx]) {
        buildEmptyState();
        return;
    }

    const auto &clips = vTracks[trackIdx]->clips();
    if (clipIdx < 0 || clipIdx >= clips.size()) {
        buildEmptyState();
        return;
    }

    auto &clip = const_cast<ClipInfo &>(clips[clipIdx]);
    const double clipDurationSeconds = clip.effectiveDuration();
    m_currentClipKey = {trackIdx, clipIdx};
    const double playheadSeconds = currentClipLocalPlayheadSeconds();
    m_motionWidget->setEnabled(true);
    m_motionWidget->setMotion(MotionState{
        clip.videoScale,
        clip.videoDx,
        clip.videoDy,
        clip.rotation2DDegrees,
        clip.opacity,
        clip.is3DLayer,
        clip.layer3D.positionZ,
        clip.layer3D.rotationX,
        clip.layer3D.rotationY,
        clip.layer3D.rotationZ
    });
    setLayerStyleControls(clip.layerStyle, true);

    // Restore motion keyframe toggle states
    for (const auto &prop : motionTrackNames()) {
        bool has = clip.keyframes.hasTrack(prop);
        m_motionWidget->setPropHasTrack(prop, has);
        m_motionWidget->setPropKeyframeTrack(prop, clip.keyframes.track(prop),
                                             clipDurationSeconds, playheadSeconds);
    }

    buildEffectGroups(clip.effects);

    // Restore effect keyframe toggle states
    for (int i = 0; i < m_effectRowWidgets.size() && i < m_effects.size(); ++i) {
        auto schema = paramSchemaFor(m_effects[i].type);
        for (const auto &def : schema) {
            const bool isColor = def.type == ParamType::Color;
            QString trackName = effectTrackName(i, def.name);
            bool has = clip.keyframes.hasTrack(trackName);
            KeyframeTrack *track = clip.keyframes.track(trackName);
            if (isColor) {
                has = hasColorChannelTrack(clip.keyframes, i, def.name);
                track = firstColorChannelTrack(clip.keyframes, i, def.name);
            }
            m_effectRowWidgets[i]->setParamHasTrack(def.name, has);
            m_effectRowWidgets[i]->setParamKeyframeTrack(def.name, track,
                                                         clipDurationSeconds, playheadSeconds);
        }
    }

    setPlayheadOnVisibleNavBars(playheadSeconds);
}

void EffectControlsPanel::buildEmptyState(const QString &message)
{
    clearContent();
    m_motionWidget->setEnabled(false);
    m_motionWidget->setMotion(MotionState{});
    setLayerStyleControls(LayerStyle{}, false);
    for (const auto &prop : motionTrackNames()) {
        m_motionWidget->setPropHasTrack(prop, false);
        m_motionWidget->setPropKeyframeTrack(prop, nullptr, 0.0, 0.0);
    }

    if (!m_emptyStateLabel) {
        m_emptyStateLabel = new QLabel("No clip selected", m_scrollContent);
        m_emptyStateLabel->setAlignment(Qt::AlignCenter);
        m_emptyStateLabel->setStyleSheet("color: #888; padding: 20px;");
    }
    m_emptyStateLabel->setText(message);
    m_scrollLayout->insertWidget(m_scrollLayout->count() - 1, m_emptyStateLabel);
    m_emptyStateLabel->show();
}

void EffectControlsPanel::setLayerStyleControls(const LayerStyle &style, bool hasClip)
{
    m_blockLayerStyleUi = true;

    const bool identity = style.isIdentity();
    const bool showShadowValues = hasClip && style.dropShadowEnabled && !identity;
    const bool showStrokeValues = hasClip && style.strokeEnabled && !identity;

    m_shadowEnable->setChecked(showShadowValues);
    m_shadowColor = showShadowValues ? style.shadowColor : QColor(0, 0, 0, 128);
    m_shadowOffsetX->setValue(showShadowValues ? style.shadowOffset.x() : 0.0);
    m_shadowOffsetY->setValue(showShadowValues ? style.shadowOffset.y() : 0.0);
    m_shadowBlurRadius->setValue(showShadowValues ? style.shadowBlurRadius : 0.0);
    const int opacityPercent = showShadowValues
        ? qBound(0, static_cast<int>(std::round(style.shadowOpacity * 100.0)), 100)
        : 0;
    m_shadowOpacitySlider->setValue(opacityPercent);
    m_shadowOpacitySpin->setValue(opacityPercent);

    m_strokeEnable->setChecked(showStrokeValues);
    m_strokeColor = showStrokeValues ? style.strokeColor : QColor(Qt::white);
    m_strokeWidth->setValue(showStrokeValues ? style.strokeWidth : 0.0);

    updateLayerStyleColorButton(m_shadowColorButton, m_shadowColor);
    updateLayerStyleColorButton(m_strokeColorButton, m_strokeColor);

    m_blockLayerStyleUi = false;
    updateLayerStyleControlAvailability(hasClip);
}

LayerStyle EffectControlsPanel::layerStyleFromControls() const
{
    LayerStyle style;
    style.dropShadowEnabled = m_shadowEnable->isChecked();
    style.shadowColor = m_shadowColor;
    style.shadowOffset = QPointF(m_shadowOffsetX->value(), m_shadowOffsetY->value());
    style.shadowBlurRadius = m_shadowBlurRadius->value();
    style.shadowOpacity = static_cast<double>(m_shadowOpacitySpin->value()) / 100.0;
    style.strokeEnabled = m_strokeEnable->isChecked();
    style.strokeColor = m_strokeColor;
    style.strokeWidth = m_strokeWidth->value();
    return style;
}

void EffectControlsPanel::updateLayerStyleControlAvailability(bool hasClip)
{
    if (!m_layerStyleGroup)
        return;

    m_layerStyleGroup->setEnabled(hasClip);
    const bool shadowEnabled = hasClip && m_shadowEnable->isChecked();
    m_shadowColorButton->setEnabled(shadowEnabled);
    m_shadowOffsetX->setEnabled(shadowEnabled);
    m_shadowOffsetY->setEnabled(shadowEnabled);
    m_shadowBlurRadius->setEnabled(shadowEnabled);
    m_shadowOpacitySlider->setEnabled(shadowEnabled);
    m_shadowOpacitySpin->setEnabled(shadowEnabled);

    const bool strokeEnabled = hasClip && m_strokeEnable->isChecked();
    m_strokeColorButton->setEnabled(strokeEnabled);
    m_strokeWidth->setEnabled(strokeEnabled);
}

void EffectControlsPanel::updateLayerStyleColorButton(QPushButton *button, const QColor &color)
{
    if (!button)
        return;
    button->setStyleSheet(QStringLiteral("background-color: %1;")
                              .arg(color.name(QColor::HexRgb)));
}

void EffectControlsPanel::persistLayerStyleFromControls()
{
    if (m_blockLayerStyleUi || !m_timeline || !m_currentClipKey.valid())
        return;
    m_timeline->setClipLayerStyle(m_currentClipKey.trackIdx,
                                  m_currentClipKey.clipIdx,
                                  layerStyleFromControls());
}

void EffectControlsPanel::chooseShadowColor()
{
    const QColor chosen = QColorDialog::getColor(
        m_shadowColor, this, QStringLiteral("影の色"), QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid())
        return;
    m_shadowColor = chosen;
    updateLayerStyleColorButton(m_shadowColorButton, m_shadowColor);
    persistLayerStyleFromControls();
}

void EffectControlsPanel::chooseStrokeColor()
{
    const QColor chosen = QColorDialog::getColor(
        m_strokeColor, this, QStringLiteral("縁取り色"), QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid())
        return;
    m_strokeColor = chosen;
    updateLayerStyleColorButton(m_strokeColorButton, m_strokeColor);
    persistLayerStyleFromControls();
}

void EffectControlsPanel::buildEffectGroups(const QVector<VideoEffect> &effects)
{
    clearContent();
    m_effects = effects;
    m_effectGroupBoxes.clear();
    m_rowToolbars.clear();
    m_effectRowWidgets.clear();

    for (int i = 0; i < effects.size(); ++i) {
        const auto &effect = effects[i];
        auto *groupBox = new QGroupBox(VideoEffect::typeName(effect.type), m_scrollContent);
        groupBox->setCheckable(false);
        m_effectGroupBoxes.append(groupBox);

        auto *layout = new QVBoxLayout(groupBox);
        layout->setContentsMargins(4, 4, 4, 4);

        auto *rowToolbar = new EffectControlsRowToolbar(i, groupBox);
        rowToolbar->setBypassed(effect.enabled);
        m_rowToolbars.append(rowToolbar);
        connect(rowToolbar, &EffectControlsRowToolbar::bypassToggled,
                this, &EffectControlsPanel::onBypassToggled);
        connect(rowToolbar, &EffectControlsRowToolbar::resetRequested,
                this, &EffectControlsPanel::onResetRequested);
        connect(rowToolbar, &EffectControlsRowToolbar::duplicateRequested,
                this, &EffectControlsPanel::onDuplicateRequested);
        connect(rowToolbar, &EffectControlsRowToolbar::removeRequested,
                this, &EffectControlsPanel::onRemoveRequested);
        connect(rowToolbar, &EffectControlsRowToolbar::moveUpRequested,
                this, &EffectControlsPanel::onMoveUpRequested);
        connect(rowToolbar, &EffectControlsRowToolbar::moveDownRequested,
                this, &EffectControlsPanel::onMoveDownRequested);

        auto *titleLayout = new QHBoxLayout();
        titleLayout->addStretch();
        titleLayout->addWidget(rowToolbar);
        layout->addLayout(titleLayout);

        auto *rowWidget = new EffectRowWidget(groupBox);
        rowWidget->setEffect(effect);
        layout->addWidget(rowWidget);
        m_effectRowWidgets.append(rowWidget);

        connect(rowWidget, &EffectRowWidget::paramChanged,
                this, [this, i](const QString &paramName, const QVariant &newValue) {
                    onParamChanged(i, paramName, newValue);
                });
        connect(rowWidget, &EffectRowWidget::keyframeToggled,
                this, [this, i](const QString &paramName, bool now) {
                    onEffectKeyframeToggled(i, paramName, now);
                });
        connect(rowWidget, &EffectRowWidget::keyframeTrackChanged,
                this, [this](const QString &) { persistEditedKeyframes(); });

        m_scrollLayout->insertWidget(m_scrollLayout->count() - 1, groupBox);
    }

    if (effects.isEmpty()) {
        if (!m_emptyStateLabel) {
            m_emptyStateLabel = new QLabel(QStringLiteral("No effects on this clip"), m_scrollContent);
            m_emptyStateLabel->setAlignment(Qt::AlignCenter);
            m_emptyStateLabel->setStyleSheet("color: #888; padding: 20px;");
        }
        m_emptyStateLabel->setText(QStringLiteral("No effects on this clip"));
        m_scrollLayout->insertWidget(m_scrollLayout->count() - 1, m_emptyStateLabel);
        m_emptyStateLabel->show();
    }
}

void EffectControlsPanel::onParamChanged(int effectIndex, const QString &paramName, const QVariant &newValue)
{
    if (effectIndex < 0 || effectIndex >= m_effects.size()) {
        return;
    }

    if (paramName == QStringLiteral("__effectStartSec")) {
        const double value = newValue.toDouble();
        m_effects[effectIndex].startSec = value < 0.0 ? -1.0 : value;
        emit effectsChanged(m_effects);
        return;
    }
    if (paramName == QStringLiteral("__effectEndSec")) {
        const double value = newValue.toDouble();
        m_effects[effectIndex].endSec = value < 0.0 ? -1.0 : value;
        emit effectsChanged(m_effects);
        return;
    }
    if (paramName == QStringLiteral("__effectTimingReset")) {
        m_effects[effectIndex].startSec = -1.0;
        m_effects[effectIndex].endSec = -1.0;
        emit effectsChanged(m_effects);
        return;
    }

    if (newValue.canConvert<double>()) {
        setParamValue(m_effects[effectIndex], paramName, newValue.toDouble());
    } else if (newValue.userType() == qMetaTypeId<QColor>()) {
        setColorParam(m_effects[effectIndex], paramName, newValue.value<QColor>());
    }

    emit effectsChanged(m_effects);
}

void EffectControlsPanel::clearContent()
{
    if (m_emptyStateLabel) {
        m_scrollLayout->removeWidget(m_emptyStateLabel);
        m_emptyStateLabel->hide();
        m_emptyStateLabel->setParent(nullptr);
        m_emptyStateLabel = nullptr;
    }

    const auto groups = m_effectGroupBoxes;
    for (auto *gb : groups) {
        m_scrollLayout->removeWidget(gb);
        gb->deleteLater();
    }

    m_effectGroupBoxes.clear();
    m_rowToolbars.clear();
    m_effectRowWidgets.clear();
}

void EffectControlsPanel::rebuildRowToolbars()
{
    for (int i = 0; i < m_rowToolbars.size(); ++i) {
        m_rowToolbars[i]->setEffectIndex(i);
    }
}

void EffectControlsPanel::onBypassToggled(int idx, bool enabled)
{
    if (idx < 0 || idx >= m_effects.size()) return;
    m_effects[idx].enabled = enabled;
    emit effectsChanged(m_effects);
}

void EffectControlsPanel::onResetRequested(int idx)
{
    if (idx < 0 || idx >= m_effects.size()) return;
    auto schema = paramSchemaFor(m_effects[idx].type);
    for (const auto &def : schema) {
        setParamValue(m_effects[idx], def.name, def.defaultVal);
    }
    m_effects[idx].startSec = -1.0;
    m_effects[idx].endSec = -1.0;
    emit effectsChanged(m_effects);
    persistAndRebuild();
}

void EffectControlsPanel::onDuplicateRequested(int idx)
{
    if (idx < 0 || idx >= m_effects.size()) return;
    if (!m_timeline) return;
    const ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey);
    if (!clip) return;

    KeyframeManager keyframes = clip->keyframes;
    duplicateEffectKeyframeTracks(keyframes, idx);
    VideoEffect copy = m_effects[idx];
    m_effects.insert(idx + 1, copy);
    m_timeline->setClipEffectsAndKeyframes(m_currentClipKey.trackIdx,
                                           m_currentClipKey.clipIdx,
                                           m_effects,
                                           keyframes);
    buildEffectGroups(m_effects);
}

void EffectControlsPanel::onRemoveRequested(int idx)
{
    if (idx < 0 || idx >= m_effects.size()) return;
    if (!m_timeline) return;
    const ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey);
    if (!clip) return;

    KeyframeManager keyframes = clip->keyframes;
    removeEffectKeyframeTracks(keyframes, idx);
    m_effects.remove(idx);
    m_timeline->setClipEffectsAndKeyframes(m_currentClipKey.trackIdx,
                                           m_currentClipKey.clipIdx,
                                           m_effects,
                                           keyframes);
    buildEffectGroups(m_effects);
}

void EffectControlsPanel::onMoveUpRequested(int idx)
{
    if (idx <= 0 || idx >= m_effects.size()) return;
    if (!m_timeline) return;
    const ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey);
    if (!clip) return;

    KeyframeManager keyframes = clip->keyframes;
    swapEffectKeyframeTracks(keyframes, idx, idx - 1);
    m_effects.swapItemsAt(idx, idx - 1);
    m_timeline->setClipEffectsAndKeyframes(m_currentClipKey.trackIdx,
                                           m_currentClipKey.clipIdx,
                                           m_effects,
                                           keyframes);
    buildEffectGroups(m_effects);
}

void EffectControlsPanel::onMoveDownRequested(int idx)
{
    if (idx < 0 || idx >= m_effects.size() - 1) return;
    if (!m_timeline) return;
    const ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey);
    if (!clip) return;

    KeyframeManager keyframes = clip->keyframes;
    swapEffectKeyframeTracks(keyframes, idx, idx + 1);
    m_effects.swapItemsAt(idx, idx + 1);
    m_timeline->setClipEffectsAndKeyframes(m_currentClipKey.trackIdx,
                                           m_currentClipKey.clipIdx,
                                           m_effects,
                                           keyframes);
    buildEffectGroups(m_effects);
}

void EffectControlsPanel::onAddEffectRequested(VideoEffectType type)
{
    VideoEffect effect;
    effect.type = type;
    effect.enabled = true;

    auto schema = paramSchemaFor(type);
    for (const auto &def : schema) {
        setParamValue(effect, def.name, def.defaultVal);
    }

    m_effects.append(effect);
    emit effectsChanged(m_effects);
    persistAndRebuild();
}

double EffectControlsPanel::currentPlayheadSeconds() const
{
    if (m_mainWindow) {
        return m_mainWindow->currentPlayheadSeconds();
    }
    if (m_timeline) {
        return m_timeline->playheadPosition();
    }
    return 0.0;
}

double EffectControlsPanel::currentClipLocalPlayheadSeconds() const
{
    const ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey);
    if (!clip) {
        return 0.0;
    }

    return boundedClipLocalSeconds(currentPlayheadSeconds(),
                                   clipTimelineStartSeconds(m_timeline, m_currentClipKey),
                                   clip->effectiveDuration());
}

void EffectControlsPanel::createKeyframeTrack(const QString &trackName, double value)
{
    ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey);
    if (!clip) {
        return;
    }

    const double t = currentClipLocalPlayheadSeconds();

    KeyframeTrack track(trackName, value);
    track.addKeyframe(t, value);
    clip->keyframes.addTrack(track);

    m_timeline->setClipKeyframes(clip->keyframes);
}

void EffectControlsPanel::removeKeyframeTrack(const QString &trackName, const QString &displayName)
{
    if (!m_timeline || !m_currentClipKey.valid()) {
        return;
    }

    auto result = QMessageBox::question(this,
        QStringLiteral("Remove Keyframes"),
        QStringLiteral("Remove all keyframes for %1?").arg(displayName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (result != QMessageBox::Yes) {
        return;
    }

    ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey);
    if (!clip) {
        return;
    }

    clip->keyframes.removeTrack(trackName);
    m_timeline->setClipKeyframes(clip->keyframes);
}

void EffectControlsPanel::onEffectKeyframeToggled(int effectIndex, const QString &paramName, bool now)
{
    if (effectIndex < 0 || effectIndex >= m_effects.size()) return;

    QString trackName = effectTrackName(effectIndex, paramName);
    QString displayName = QStringLiteral("%1: %2").arg(VideoEffect::typeName(m_effects[effectIndex].type)).arg(paramName);

    if (isColorParam(m_effects[effectIndex].type, paramName)) {
        const ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey);
        if (!clip) {
            refreshFromCurrentClip();
            return;
        }

        KeyframeManager keyframes = clip->keyframes;
        if (now) {
            QColor color = colorParamValue(m_effects[effectIndex], paramName);
            if (!color.isValid())
                color = QColor(0, 0, 0);

            const double t = currentClipLocalPlayheadSeconds();
            for (const QString &channel : colorChannelNames()) {
                const QString channelTrackName =
                    effectColorChannelTrackName(effectIndex, paramName, channel);
                const double value = colorChannelValue(color, channel);
                KeyframeTrack channelTrack(channelTrackName, value);
                channelTrack.addKeyframe(t, value);
                keyframes.addTrack(channelTrack);
            }
            m_timeline->setClipEffectsAndKeyframes(m_currentClipKey.trackIdx,
                                                   m_currentClipKey.clipIdx,
                                                   m_effects,
                                                   keyframes);
        } else {
            auto result = QMessageBox::question(
                this,
                QStringLiteral("Remove Keyframes"),
                QStringLiteral("Remove all keyframes for %1 RGB?").arg(displayName),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (result != QMessageBox::Yes) {
                refreshFromCurrentClip();
                return;
            }

            for (const QString &channel : colorChannelNames()) {
                keyframes.removeTrack(effectColorChannelTrackName(effectIndex, paramName, channel));
            }
            m_timeline->setClipEffectsAndKeyframes(m_currentClipKey.trackIdx,
                                                   m_currentClipKey.clipIdx,
                                                   m_effects,
                                                   keyframes);
        }
        refreshFromCurrentClip();
        return;
    }

    if (now) {
        double value = 0.0;
        if (effectIndex < m_effectRowWidgets.size()) {
            value = m_effectRowWidgets[effectIndex]->getParamValueByName(paramName);
        }
        createKeyframeTrack(trackName, value);
    } else {
        removeKeyframeTrack(trackName, displayName);
    }
    refreshFromCurrentClip();
}

void EffectControlsPanel::onMotionKeyframeToggled(const QString &propPath, bool now)
{
    if (now) {
        double value = m_motionWidget->getMotionValue(propPath);
        createKeyframeTrack(propPath, value);
    } else {
        QString displayName = propPath;
        removeKeyframeTrack(propPath, displayName);
    }
    refreshFromCurrentClip();
}

void EffectControlsPanel::setPlayheadOnVisibleNavBars(double seconds)
{
    m_motionWidget->setPlayhead(seconds);
    for (auto *rowWidget : m_effectRowWidgets) {
        if (rowWidget) {
            rowWidget->setPlayhead(seconds);
        }
    }
}

void EffectControlsPanel::persistEditedKeyframes()
{
    ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey);
    if (!clip) {
        return;
    }

    m_timeline->setClipKeyframes(clip->keyframes);
    refreshFromCurrentClip();
}

void EffectControlsPanel::persistAndRebuild()
{
    if (m_timeline && m_currentClipKey.valid()) {
        m_timeline->setClipEffects(m_effects);
    }
    buildEffectGroups(m_effects);
}

} // namespace effectctrl
