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
#include <QScrollBar>
#include <QMessageBox>

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

bool splitEffectTrackName(const QString &trackName, int &effectIndex, QString &paramName)
{
    static const QString prefix = QStringLiteral("effect.");
    if (!trackName.startsWith(prefix)) {
        return false;
    }

    const int indexStart = prefix.size();
    const int dot = trackName.indexOf(QLatin1Char('.'), indexStart);
    if (dot <= indexStart || dot == trackName.size() - 1) {
        return false;
    }

    bool ok = false;
    const int parsedIndex = trackName.mid(indexStart, dot - indexStart).toInt(&ok);
    if (!ok || parsedIndex < 0) {
        return false;
    }

    effectIndex = parsedIndex;
    paramName = trackName.mid(dot + 1);
    return true;
}

QString effectTrackName(int effectIndex, const QString &paramName)
{
    return QStringLiteral("effect.%1.%2").arg(effectIndex).arg(paramName);
}

KeyframeTrack renamedTrack(const KeyframeTrack &track, const QString &newName)
{
    KeyframeTrack renamed(newName, track.defaultValue());
    for (const auto &kf : track.keyframes()) {
        renamed.addKeyframe(kf.time, kf.value, kf.interpolation);
    }
    return renamed;
}

bool remapEffectKeyframeTracks(ClipInfo *clip, const QVector<int> &oldToNew,
                               int duplicateFrom = -1, int duplicateTo = -1)
{
    if (!clip || oldToNew.isEmpty()) {
        return false;
    }

    const QVector<KeyframeTrack> originalTracks = clip->keyframes.tracks();
    QVector<KeyframeTrack> remappedTracks;
    remappedTracks.reserve(originalTracks.size());
    bool changed = false;

    for (const auto &track : originalTracks) {
        int effectIndex = -1;
        QString paramName;
        if (!splitEffectTrackName(track.propertyName(), effectIndex, paramName)
            || effectIndex >= oldToNew.size()) {
            remappedTracks.append(track);
            continue;
        }

        const int newIndex = oldToNew[effectIndex];
        if (newIndex < 0) {
            changed = true;
            continue;
        }

        const QString newName = effectTrackName(newIndex, paramName);
        if (newName == track.propertyName()) {
            remappedTracks.append(track);
        } else {
            remappedTracks.append(renamedTrack(track, newName));
            changed = true;
        }
    }

    if (duplicateFrom >= 0 && duplicateTo >= 0) {
        for (const auto &track : originalTracks) {
            int effectIndex = -1;
            QString paramName;
            if (splitEffectTrackName(track.propertyName(), effectIndex, paramName)
                && effectIndex == duplicateFrom) {
                remappedTracks.append(renamedTrack(track, effectTrackName(duplicateTo, paramName)));
                changed = true;
            }
        }
    }

    if (changed) {
        clip->keyframes.tracks() = remappedTracks;
    }
    return changed;
}

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
            [this](double seconds) { setPlayheadOnVisibleNavBars(seconds); });
    setPlayheadOnVisibleNavBars(currentPlayheadSeconds());
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
    const double playheadSeconds = currentPlayheadSeconds();
    m_currentClipKey = {trackIdx, clipIdx};
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
            QString trackName = QStringLiteral("effect.%1.%2").arg(i).arg(def.name);
            bool has = clip.keyframes.hasTrack(trackName);
            m_effectRowWidgets[i]->setParamHasTrack(def.name, has);
            m_effectRowWidgets[i]->setParamKeyframeTrack(def.name, clip.keyframes.track(trackName),
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
    for (const auto &prop : motionTrackNames()) {
        m_motionWidget->setPropHasTrack(prop, false);
        m_motionWidget->setPropKeyframeTrack(prop, nullptr, 0.0, currentPlayheadSeconds());
    }

    if (!m_emptyStateLabel) {
        m_emptyStateLabel = new QLabel("No clip selected", m_scrollContent);
        m_emptyStateLabel->setAlignment(Qt::AlignCenter);
        m_emptyStateLabel->setStyleSheet("color: #888; padding: 20px;");
    }
    m_emptyStateLabel->setText(message);
    m_scrollLayout->insertWidget(1, m_emptyStateLabel);
    m_emptyStateLabel->show();
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
        m_scrollLayout->insertWidget(1, m_emptyStateLabel);
        m_emptyStateLabel->show();
    }
}

void EffectControlsPanel::onParamChanged(int effectIndex, const QString &paramName, const QVariant &newValue)
{
    if (effectIndex < 0 || effectIndex >= m_effects.size()) {
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

    auto children = m_scrollContent->findChildren<QGroupBox *>();
    for (auto *gb : children) {
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
    emit effectsChanged(m_effects);
    persistAndRebuild();
}

void EffectControlsPanel::onDuplicateRequested(int idx)
{
    if (idx < 0 || idx >= m_effects.size()) return;
    const int oldCount = m_effects.size();
    QVector<int> oldToNew(oldCount);
    for (int i = 0; i < oldCount; ++i) {
        oldToNew[i] = i > idx ? i + 1 : i;
    }
    VideoEffect copy = m_effects[idx];
    m_effects.insert(idx + 1, copy);
    if (ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey)) {
        if (remapEffectKeyframeTracks(clip, oldToNew, idx, idx + 1)) {
            m_timeline->setClipKeyframes(clip->keyframes);
        }
    }
    emit effectsChanged(m_effects);
    persistAndRebuild();
}

void EffectControlsPanel::onRemoveRequested(int idx)
{
    if (idx < 0 || idx >= m_effects.size()) return;
    const int oldCount = m_effects.size();
    QVector<int> oldToNew(oldCount);
    for (int i = 0; i < oldCount; ++i) {
        oldToNew[i] = i == idx ? -1 : (i > idx ? i - 1 : i);
    }
    m_effects.remove(idx);
    if (ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey)) {
        if (remapEffectKeyframeTracks(clip, oldToNew)) {
            m_timeline->setClipKeyframes(clip->keyframes);
        }
    }
    emit effectsChanged(m_effects);
    persistAndRebuild();
}

void EffectControlsPanel::onMoveUpRequested(int idx)
{
    if (idx <= 0 || idx >= m_effects.size()) return;
    QVector<int> oldToNew(m_effects.size());
    for (int i = 0; i < oldToNew.size(); ++i) {
        oldToNew[i] = i;
    }
    oldToNew[idx] = idx - 1;
    oldToNew[idx - 1] = idx;
    m_effects.swapItemsAt(idx, idx - 1);
    if (ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey)) {
        if (remapEffectKeyframeTracks(clip, oldToNew)) {
            m_timeline->setClipKeyframes(clip->keyframes);
        }
    }
    emit effectsChanged(m_effects);
    persistAndRebuild();
}

void EffectControlsPanel::onMoveDownRequested(int idx)
{
    if (idx < 0 || idx >= m_effects.size() - 1) return;
    QVector<int> oldToNew(m_effects.size());
    for (int i = 0; i < oldToNew.size(); ++i) {
        oldToNew[i] = i;
    }
    oldToNew[idx] = idx + 1;
    oldToNew[idx + 1] = idx;
    m_effects.swapItemsAt(idx, idx + 1);
    if (ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey)) {
        if (remapEffectKeyframeTracks(clip, oldToNew)) {
            m_timeline->setClipKeyframes(clip->keyframes);
        }
    }
    emit effectsChanged(m_effects);
    persistAndRebuild();
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

void EffectControlsPanel::createKeyframeTrack(const QString &trackName, double value)
{
    ClipInfo *clip = selectedClipForKey(m_timeline, m_currentClipKey);
    if (!clip) {
        return;
    }

    const double t = qBound(0.0, currentPlayheadSeconds(), clip->effectiveDuration());

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

    QString trackName = QStringLiteral("effect.%1.%2").arg(effectIndex).arg(paramName);
    QString displayName = QStringLiteral("%1: %2").arg(VideoEffect::typeName(m_effects[effectIndex].type)).arg(paramName);

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
