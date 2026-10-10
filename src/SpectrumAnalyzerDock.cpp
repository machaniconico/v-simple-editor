#include "SpectrumAnalyzerDock.h"
#include "AudioMixer.h"

#include <QDateTime>
#include <QHideEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kFloorDb = -120.0f;

// A pure radix-2 FFT. The Hann coherent gain makes a bin-centred sine's
// magnitude read as its input amplitude; the floor keeps silence finite.
void fftMagnitudes(const QVector<float> &mono, QVector<float> *outDb)
{
    constexpr int n = SpectrumAnalyzerDock::kFftSize;
    std::vector<std::complex<double>> bins(n);
    double windowSum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
        const double sample = i < mono.size() && std::isfinite(mono[i]) ? mono[i] : 0.0;
        bins[i] = sample * w;
        windowSum += w;
    }
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(bins[i], bins[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / len;
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (int start = 0; start < n; start += len) {
            std::complex<double> phase(1.0, 0.0);
            for (int j = 0; j < len / 2; ++j) {
                const auto even = bins[start + j];
                const auto odd = phase * bins[start + j + len / 2];
                bins[start + j] = even + odd;
                bins[start + j + len / 2] = even - odd;
                phase *= step;
            }
        }
    }
    outDb->resize(n / 2 + 1);
    for (int i = 0; i <= n / 2; ++i) {
        const double amplitude = std::abs(bins[i]) * (i == 0 || i == n / 2 ? 1.0 : 2.0) / windowSum;
        (*outDb)[i] = static_cast<float>(20.0 * std::log10(std::max(amplitude, 1.0e-6)));
    }
}

class SpectrumView final : public QWidget
{
public:
    explicit SpectrumView(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("spectrumAnalyzerView"));
        setMinimumSize(320, 160);
    }

    void setSpectrum(const QVector<float> &db, const QVector<float> &hold)
    {
        m_db = db;
        m_hold = hold;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(22, 26, 32));
        const QRectF plot = rect().adjusted(42, 12, -12, -26);
        if (plot.width() <= 0 || plot.height() <= 0) return;
        const auto xForHz = [&plot](double hz) {
            return plot.left() + plot.width() * std::log(hz / 20.0) / std::log(1000.0);
        };
        const auto yForDb = [&plot](float db) {
            return plot.bottom() - plot.height() * qBound(0.0, (double(db) + 90.0) / 90.0, 1.0);
        };
        painter.setPen(QColor(68, 74, 82));
        for (int hz : {20, 100, 1000, 10000, 20000}) {
            const double x = xForHz(hz);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            painter.drawText(QPointF(x + 2, height() - 7), hz >= 1000
                ? QStringLiteral("%1k").arg(hz / 1000) : QString::number(hz));
        }
        for (int db : {-90, -60, -30, 0}) {
            const double y = yForDb(float(db));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.drawText(QPointF(3, y + 4), QString::number(db));
        }
        const auto pathFor = [&](const QVector<float> &values) {
            QPainterPath path;
            bool started = false;
            for (int bin = 1; bin < values.size(); ++bin) {
                const double hz = double(bin) * SpectrumAnalyzerDock::kSampleRateHz
                    / SpectrumAnalyzerDock::kFftSize;
                if (hz < 20.0 || hz > 20000.0) continue;
                const QPointF point(xForHz(hz), yForDb(values[bin]));
                if (!started) { path.moveTo(point); started = true; }
                else path.lineTo(point);
            }
            return path;
        };
        painter.setClipRect(plot);
        painter.setPen(QPen(QColor(255, 178, 67, 140), 1.0));
        painter.drawPath(pathFor(m_hold));
        painter.setPen(QPen(QColor(79, 218, 179), 1.5));
        painter.drawPath(pathFor(m_db));
    }

private:
    QVector<float> m_db;
    QVector<float> m_hold;
};

} // namespace

SpectrumAnalyzerDock::SpectrumAnalyzerDock(AudioMixer *mixer, QWidget *parent)
    : QDockWidget(QStringLiteral("スペクトラムアナライザ"), parent), m_mixer(mixer)
{
    setObjectName(QStringLiteral("spectrumAnalyzerDock"));
    m_view = new SpectrumView(this);
    setWidget(m_view);
    m_redrawTimer = new QTimer(this);
    m_redrawTimer->setInterval(34); // <= 30 frames per second
    connect(m_redrawTimer, &QTimer::timeout, this, &SpectrumAnalyzerDock::drawPendingFrame);
    if (m_mixer)
        connect(m_mixer, &AudioMixer::masterTapAvailable,
                this, &SpectrumAnalyzerDock::onMasterTapAvailable);
    hide();
}

SpectrumAnalyzerDock::~SpectrumAnalyzerDock()
{
    unsubscribe();
}

QVector<float> SpectrumAnalyzerDock::computeSpectrumForTest(const QVector<float> &mono)
{
    QVector<float> out;
    fftMagnitudes(mono, &out);
    return out;
}

void SpectrumAnalyzerDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    if (m_mixer && !m_subscribed) {
        m_mixer->subscribeMasterTap();
        m_subscribed = true;
    }
    m_redrawTimer->start();
}

void SpectrumAnalyzerDock::hideEvent(QHideEvent *event)
{
    m_redrawTimer->stop();
    unsubscribe();
    QDockWidget::hideEvent(event);
}

void SpectrumAnalyzerDock::unsubscribe()
{
    if (m_subscribed && m_mixer) m_mixer->unsubscribeMasterTap();
    m_subscribed = false;
    m_mono.clear();
}

void SpectrumAnalyzerDock::onMasterTapAvailable()
{
    if (!m_subscribed || !m_mixer) return;
    QVector<float> stereo;
    const int frames = m_mixer->takeMasterTap(&stereo, AudioMixer::kMasterTapFrames);
    if (frames <= 0) return;
    m_mono.reserve(kFftSize);
    for (int i = 0; i < frames; ++i)
        m_mono.append(0.5f * (stereo[2 * i] + stereo[2 * i + 1]));
    if (m_mono.size() > kFftSize)
        m_mono.remove(0, m_mono.size() - kFftSize);
    if (m_mono.size() < kFftSize) return;
    fftMagnitudes(m_mono, &m_lastSpectrum);
    ++m_spectrumUpdates;
    m_dirty = true;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_peakHold.size() != m_lastSpectrum.size()) {
        m_peakHold.fill(kFloorDb, m_lastSpectrum.size());
        m_peakUpdatedMs.fill(nowMs, m_lastSpectrum.size());
    }
    for (int i = 0; i < m_lastSpectrum.size(); ++i) {
        const float decayed = m_peakHold[i] - 12.0f
            * std::max<qint64>(0, nowMs - m_peakUpdatedMs[i] - 1000) / 1000.0f;
        if (m_lastSpectrum[i] >= decayed) {
            m_peakHold[i] = m_lastSpectrum[i];
            m_peakUpdatedMs[i] = nowMs;
        }
    }
}

void SpectrumAnalyzerDock::drawPendingFrame()
{
    if (m_lastSpectrum.isEmpty()) return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QVector<float> visibleHold(m_peakHold.size());
    bool fading = false;
    for (int i = 0; i < visibleHold.size(); ++i) {
        visibleHold[i] = std::max(kFloorDb, m_peakHold[i] - 12.0f
            * std::max<qint64>(0, nowMs - m_peakUpdatedMs[i] - 1000) / 1000.0f);
        fading |= visibleHold[i] > kFloorDb && visibleHold[i] < m_peakHold[i];
    }
    if (!m_dirty && !fading) return;
    static_cast<SpectrumView *>(m_view)->setSpectrum(m_lastSpectrum, visibleHold);
    m_dirty = false;
}
