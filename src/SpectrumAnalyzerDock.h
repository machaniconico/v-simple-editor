#pragma once

#include <QDockWidget>
#include <QPointer>
#include <QVector>

class AudioMixer;
class QHideEvent;
class QShowEvent;
class QTimer;
class QWidget;

// Display-only view of the 48 kHz master bus. It never changes audio samples.
class SpectrumAnalyzerDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit SpectrumAnalyzerDock(AudioMixer *mixer, QWidget *parent = nullptr);
    ~SpectrumAnalyzerDock() override;

    static constexpr int kFftSize = 2048;
    static constexpr int kSampleRateHz = 48000;
    static QVector<float> computeSpectrumForTest(const QVector<float> &mono);
    QVector<float> lastSpectrumForTest() const { return m_lastSpectrum; }
    quint64 spectrumUpdateCountForTest() const { return m_spectrumUpdates; }
    bool tapSubscribedForTest() const { return m_subscribed; }

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void onMasterTapAvailable();
    void drawPendingFrame();
    void unsubscribe();

    QPointer<AudioMixer> m_mixer;
    QWidget *m_view = nullptr;
    QTimer *m_redrawTimer = nullptr;
    QVector<float> m_mono;
    QVector<float> m_lastSpectrum;
    QVector<float> m_peakHold;
    QVector<qint64> m_peakUpdatedMs;
    quint64 m_spectrumUpdates = 0;
    bool m_subscribed = false;
    bool m_dirty = false;
};
