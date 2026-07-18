#include "SpectrogramWidget.h"

#include <QImage>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRect>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// 値 v を [lo, hi] にクランプする小ヘルパー。
template <typename T>
T clampVal(T v, T lo, T hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// fftSize の妥当性チェック (2 のべき乗かつ >= 2)。
bool isPowerOfTwo(int n)
{
    return n >= 2 && (n & (n - 1)) == 0;
}

} // namespace

SpectrogramWidget::SpectrogramWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 180);
    setMouseTracking(false);
    // 暗い背景の方がスペクトログラムが見やすい。
    setAutoFillBackground(true);
}

void SpectrogramWidget::setFftSize(int fftSize)
{
    if (isPowerOfTwo(fftSize)) {
        m_fftSize = fftSize;
        m_hopSize = std::max(1, fftSize / 4);
    }
}

void SpectrogramWidget::setAttenuation(double attenuation)
{
    m_attenuation = clampVal(attenuation, 0.0, 1.0);
}

void SpectrogramWidget::setSamples(const std::vector<double> &samples, int sampleRate)
{
    m_samples    = samples;
    m_sampleRate = sampleRate;

    // 選択は新しい音声で意味が変わるためクリアする。
    m_hasSelection = false;
    m_selecting    = false;
    m_selectionRect = QRect();

    rebuildImage();
    update();
    emit selectionChanged();
}

void SpectrogramWidget::rebuildImage()
{
    m_image = QImage();
    m_numFrames   = 0;
    m_durationSec = 0.0;
    m_maxFreqHz   = 0.0;

    if (m_samples.empty() || m_sampleRate <= 0 || !isPowerOfTwo(m_fftSize))
        return;

    const spectral::Stft s =
        spectral::stft(m_samples, m_sampleRate, m_fftSize, m_hopSize);
    if (s.frames.empty() || s.fftSize <= 0)
        return;

    m_numFrames   = static_cast<int>(s.frames.size());
    m_durationSec = static_cast<double>(m_samples.size()) / m_sampleRate;
    m_maxFreqHz   = m_sampleRate / 2.0;

    // 表示するビンは 0..fftSize/2 (Nyquist まで)。縦方向の高さに対応する。
    const int halfBins = m_fftSize / 2 + 1;

    // まずマグニチュード (dB) を計算し、最大値を求めて正規化に使う。
    std::vector<double> mags(static_cast<size_t>(m_numFrames) * halfBins, 0.0);
    double maxDb = -1e300;
    for (int f = 0; f < m_numFrames; ++f) {
        const auto &frame = s.frames[f];
        for (int k = 0; k < halfBins; ++k) {
            const double mag = std::abs(frame[k]);
            // 20*log10(mag) 相当。0 を避けるため小さい floor を足す。
            const double db = 20.0 * std::log10(mag + 1e-9);
            mags[static_cast<size_t>(f) * halfBins + k] = db;
            if (db > maxDb)
                maxDb = db;
        }
    }
    if (maxDb <= -1e299)
        maxDb = 0.0;

    // dB のダイナミックレンジ (この幅を色強度 0..1 にマップする)。
    const double dynRangeDb = 80.0;
    const double minDb = maxDb - dynRangeDb;

    // QImage は横=フレーム(時間), 縦=ビン(周波数)。縦は上が高域になるよう反転する。
    QImage img(m_numFrames, halfBins, QImage::Format_RGB32);
    for (int f = 0; f < m_numFrames; ++f) {
        for (int k = 0; k < halfBins; ++k) {
            const double db = mags[static_cast<size_t>(f) * halfBins + k];
            double norm = (db - minDb) / dynRangeDb;
            norm = clampVal(norm, 0.0, 1.0);

            // 簡易な "magma" 風グラデーション (黒→紫→橙→白)。
            const int r = static_cast<int>(clampVal(norm * 1.6, 0.0, 1.0) * 255.0);
            const int g = static_cast<int>(clampVal((norm - 0.35) * 1.8, 0.0, 1.0) * 255.0);
            const int b = static_cast<int>(clampVal(std::sin(norm * M_PI) * 0.9, 0.0, 1.0) * 255.0);

            // 縦反転: k=0 (低域) を最下行に置く。
            img.setPixel(f, halfBins - 1 - k, qRgb(r, g, b));
        }
    }

    m_image = img;
}

QRect SpectrogramWidget::plotRect() const
{
    return contentsRect();
}

double SpectrogramWidget::xToTimeSec(int x) const
{
    const QRect r = plotRect();
    if (r.width() <= 0 || m_durationSec <= 0.0)
        return 0.0;
    const double frac = clampVal(
        static_cast<double>(x - r.left()) / r.width(), 0.0, 1.0);
    return frac * m_durationSec;
}

double SpectrogramWidget::yToFreqHz(int y) const
{
    const QRect r = plotRect();
    if (r.height() <= 0 || m_maxFreqHz <= 0.0)
        return 0.0;
    // 上端が高域、下端が低域 (描画と同じ向き)。
    const double frac = clampVal(
        static_cast<double>(r.bottom() - y) / r.height(), 0.0, 1.0);
    return frac * m_maxFreqHz;
}

std::vector<spectral::SpectralRegion> SpectrogramWidget::selectedRegions() const
{
    std::vector<spectral::SpectralRegion> regions;
    if (!m_hasSelection || m_selectionRect.isEmpty() ||
        m_samples.empty() || m_sampleRate <= 0 ||
        m_image.isNull() || m_durationSec <= 0.0 || m_maxFreqHz <= 0.0)
        return regions;

    const QRect sel = m_selectionRect.normalized();

    spectral::SpectralRegion region;
    region.tStartSec   = xToTimeSec(sel.left());
    region.tEndSec     = xToTimeSec(sel.right());
    region.fLowHz      = yToFreqHz(sel.bottom()); // 下端=低域
    region.fHighHz     = yToFreqHz(sel.top());    // 上端=高域
    region.attenuation = m_attenuation;

    // 念のため start<=end / low<=high を保証する。
    if (region.tEndSec < region.tStartSec)
        std::swap(region.tStartSec, region.tEndSec);
    if (region.fHighHz < region.fLowHz)
        std::swap(region.fLowHz, region.fHighHz);

    regions.push_back(region);
    return regions;
}

void SpectrogramWidget::clearSelection()
{
    if (!m_hasSelection && !m_selecting)
        return;
    m_hasSelection  = false;
    m_selecting     = false;
    m_selectionRect = QRect();
    update();
    emit selectionChanged();
}

void SpectrogramWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    const QRect r = plotRect();

    // 背景。
    painter.fillRect(rect(), QColor(20, 20, 28));

    if (!m_image.isNull()) {
        // スペクトログラムをプロット矩形いっぱいに拡大描画する。
        painter.drawImage(r, m_image, m_image.rect());
    } else {
        // samples 未設定時の空表示。
        painter.setPen(QColor(140, 140, 150));
        painter.drawText(r, Qt::AlignCenter,
                         tr("音声を読み込むとスペクトログラムが表示されます"));
    }

    // 確定済み選択矩形 / ドラッグ中の矩形をオーバーレイ描画する。
    QRect overlay;
    if (m_selecting)
        overlay = QRect(m_dragStart, m_dragEnd).normalized();
    else if (m_hasSelection)
        overlay = m_selectionRect.normalized();

    if (!overlay.isEmpty()) {
        QColor fill(80, 160, 255, 60);
        painter.fillRect(overlay, fill);
        painter.setPen(QPen(QColor(120, 190, 255), 1));
        painter.drawRect(overlay);
    }
}

void SpectrogramWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_image.isNull()) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_selecting    = true;
    m_hasSelection = false;
    m_dragStart    = event->position().toPoint();
    m_dragEnd      = m_dragStart;
    update();
}

void SpectrogramWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_selecting) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    m_dragEnd = event->position().toPoint();
    update();
}

void SpectrogramWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_selecting) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_selecting = false;
    m_dragEnd   = event->position().toPoint();

    const QRect raw = QRect(m_dragStart, m_dragEnd).normalized();
    // プロット矩形でクリップする。
    const QRect clipped = raw.intersected(plotRect());

    // あまりに小さいドラッグ (誤クリック) は選択扱いにしない。
    if (clipped.width() >= 3 && clipped.height() >= 3) {
        m_selectionRect = clipped;
        m_hasSelection  = true;
    } else {
        m_hasSelection  = false;
        m_selectionRect = QRect();
    }

    update();
    emit selectionChanged();
}
