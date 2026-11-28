#include "DeEsserPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSignalBlocker>

#include <climits>
#include <iterator>

namespace {
constexpr int kNoTrack = INT_MIN; // kMasterTrackId (-1) is a valid target
}

DeEsserPanel::DeEsserPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("DeEsserPanel"));
    auto *form = new QFormLayout(this);

    m_target = new QComboBox(this);
    m_target->setObjectName(QStringLiteral("deesser_target"));
    form->addRow(tr("対象"), m_target);

    m_enabled = new QCheckBox(tr("有効"), this);
    m_enabled->setObjectName(QStringLiteral("deesser_enabled"));
    form->addRow(m_enabled);

    const auto spin = [&](const char *name, const QString &label, double low,
                          double high, double step, const QString &suffix) {
        auto *box = new QDoubleSpinBox(this);
        box->setObjectName(QString::fromLatin1(name));
        box->setRange(low, high);
        box->setSingleStep(step);
        box->setSuffix(suffix);
        form->addRow(label, box);
        connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &DeEsserPanel::onAnyControlChanged);
        // Typing / stepping is one gesture, finished by Enter or focus loss.
        connect(box, &QDoubleSpinBox::editingFinished, this, &DeEsserPanel::finishGesture);
        return box;
    };
    m_frequency = spin("deesser_frequency", tr("周波数"), 1000.0, 16000.0, 100.0, tr(" Hz"));
    m_threshold = spin("deesser_threshold", tr("しきい値"), -60.0, 0.0, 1.0, tr(" dB"));
    m_ratio = spin("deesser_ratio", tr("レシオ"), 1.0, 20.0, 0.5, QStringLiteral(":1"));
    m_range = spin("deesser_range", tr("レンジ（最大減衰量）"), 0.0, 24.0, 1.0, tr(" dB"));

    m_listen = new QCheckBox(tr("リッスン（検出帯域のみ）"), this);
    m_listen->setObjectName(QStringLiteral("deesser_listen"));
    form->addRow(m_listen);

    connect(m_enabled, &QCheckBox::toggled, this, &DeEsserPanel::onAnyControlChanged);
    connect(m_enabled, &QCheckBox::toggled, this, &DeEsserPanel::finishGesture);
    connect(m_listen, &QCheckBox::toggled, this, &DeEsserPanel::onAnyControlChanged);
    connect(m_target, &QComboBox::currentIndexChanged, this, &DeEsserPanel::onTargetChanged);
    showSettings(trackfx::DeEsserSettings{});
}

void DeEsserPanel::showSettings(const trackfx::DeEsserSettings &s)
{
    const bool previous = m_suppressSignals;
    m_suppressSignals = true;
    m_enabled->setChecked(s.enabled);
    m_listen->setChecked(s.listen);
    m_frequency->setValue(s.frequencyHz);
    m_threshold->setValue(s.thresholdDb);
    m_ratio->setValue(s.ratio);
    m_range->setValue(s.rangeDb);
    m_suppressSignals = previous;
}

void DeEsserPanel::setTracks(const QStringList &itemNames, const QList<int> &trackIds)
{
    const int previousId = m_currentTrackId;
    {
        const QSignalBlocker blocker(m_target);
        m_target->clear();
        for (int i = 0; i < qMin(itemNames.size(), trackIds.size()); ++i)
            m_target->addItem(itemNames[i], trackIds[i]);
        const int index = m_target->findData(previousId);
        m_target->setCurrentIndex(index >= 0 ? index : (m_target->count() > 0 ? 0 : -1));
    }
    m_currentTrackId = m_target->currentIndex() >= 0 ? m_target->currentData().toInt() : kNoTrack;
    setEnabled(m_target->count() > 0);
    for (auto it = m_perTrackSettings.begin(); it != m_perTrackSettings.end();)
        it = trackIds.contains(it.key()) ? std::next(it) : m_perTrackSettings.erase(it);
    showSettings(m_perTrackSettings.value(m_currentTrackId));
}

void DeEsserPanel::setSettings(int trackId, const trackfx::DeEsserSettings &settings)
{
    m_perTrackSettings.insert(trackId, settings);
    if (trackId == m_currentTrackId)
        showSettings(settings);
}

int DeEsserPanel::targetCount() const
{
    return m_target->count();
}

int DeEsserPanel::currentTrackId() const
{
    return m_currentTrackId;
}

trackfx::DeEsserSettings DeEsserPanel::currentSettings() const
{
    trackfx::DeEsserSettings s;
    s.enabled = m_enabled->isChecked();
    s.listen = m_listen->isChecked();
    s.frequencyHz = m_frequency->value();
    s.thresholdDb = m_threshold->value();
    s.ratio = m_ratio->value();
    s.rangeDb = m_range->value();
    return s;
}

void DeEsserPanel::onTargetChanged()
{
    // Selection alone must never write to the mixer: keep the values of the
    // target being left, then show the stored values of the new one silently.
    if (m_currentTrackId != kNoTrack)
        m_perTrackSettings.insert(m_currentTrackId, currentSettings());
    m_currentTrackId = m_target->currentIndex() >= 0 ? m_target->currentData().toInt() : kNoTrack;
    showSettings(m_perTrackSettings.value(m_currentTrackId));
}

void DeEsserPanel::onAnyControlChanged()
{
    if (m_suppressSignals || m_currentTrackId == kNoTrack) return;
    const auto settings = currentSettings();
    m_perTrackSettings.insert(m_currentTrackId, settings);
    if (sender() != m_listen) m_gestureDirty = true;
    emit settingsChanged(m_currentTrackId, settings);
}

void DeEsserPanel::finishGesture()
{
    if (!m_gestureDirty || m_suppressSignals || m_currentTrackId == kNoTrack) return;
    m_gestureDirty = false;
    emit gestureFinished(m_currentTrackId);
}
