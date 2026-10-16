#pragma once

#include <QDockWidget>
#include <QPointer>
#include <QVector>

class AudioMixer;
class QComboBox;
class QHideEvent;
class QShowEvent;
class QTimer;
class QWidget;
class Timeline;

// Display-only view of the 48 kHz master bus or of one audio track after its
// FX and before the fader (spectrumSourceCombo). It never changes audio samples.
class SpectrumAnalyzerDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit SpectrumAnalyzerDock(AudioMixer *mixer, QWidget *parent = nullptr);
    ~SpectrumAnalyzerDock() override;
    // Lists the timeline's audio tracks (A1..An) as tap sources and follows
    // track count changes and remaps (MainWindow wires its Timeline).
    void setTimeline(Timeline *timeline);

    static constexpr int kFftSize = 2048;
    static constexpr int kSampleRateHz = 48000;
    static constexpr float kFloorDb = -120.0f;
    // Once no tap has arrived for kIdleHoldMs (stopped transport), the shown
    // spectrum falls by kIdleDecayDbPerSec per redraw tick down to kFloorDb.
    static constexpr int kIdleHoldMs = 1000;
    static constexpr double kIdleDecayDbPerSec = 12.0;
    static constexpr int kRedrawIntervalMs = 34;
    static QVector<float> computeSpectrumForTest(const QVector<float> &mono);
    QVector<float> lastSpectrumForTest() const { return m_lastSpectrum; }
    quint64 spectrumUpdateCountForTest() const { return m_spectrumUpdates; }
    bool tapSubscribedForTest() const { return m_subscribed; }
    // Runs `steps` idle redraw ticks of the decay (kRedrawIntervalMs each)
    // without the kIdleHoldMs wait.
    void idleDecayStepsForTest(int steps);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void onMasterTapAvailable();
    void drawPendingFrame();
    void unsubscribe();
    void rebuildSourceCombo();
    void decayIdle(double seconds);

    QPointer<AudioMixer> m_mixer;
    QPointer<Timeline> m_timeline;
    QComboBox *m_sourceCombo = nullptr;
    QWidget *m_view = nullptr;
    QTimer *m_redrawTimer = nullptr;
    QVector<float> m_mono;
    QVector<float> m_lastSpectrum;
    QVector<float> m_peakHold;
    QVector<qint64> m_peakUpdatedMs;
    quint64 m_spectrumUpdates = 0;
    qint64 m_lastTapMs = 0;
    qint64 m_lastDecayMs = 0;
    bool m_subscribed = false;
    bool m_dirty = false;
};
