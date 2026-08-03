#include "Deflicker.h"

#include <QRectF>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

namespace deflicker {

QRect effectiveAnalysisRegion(const QImage &frame,
                              const DeflickerParams &params)
{
    if (frame.isNull())
        return {};

    const QRect full = frame.rect();
    if (params.analysisRegion.isEmpty())
        return full;
    return params.analysisRegion.intersected(full);
}

QRect scaleAnalysisRegion(const QRect &canvasRegion,
                          const QSize &canvasSize,
                          const QSize &frameSize)
{
    if (canvasRegion.isEmpty() || frameSize.isEmpty())
        return {};
    if (canvasSize.width() <= 0 || canvasSize.height() <= 0)
        return canvasRegion;

    const double scaleX = static_cast<double>(frameSize.width())
        / static_cast<double>(canvasSize.width());
    const double scaleY = static_cast<double>(frameSize.height())
        / static_cast<double>(canvasSize.height());
    return QRectF(canvasRegion.x() * scaleX,
                  canvasRegion.y() * scaleY,
                  canvasRegion.width() * scaleX,
                  canvasRegion.height() * scaleY).toAlignedRect();
}

namespace {

constexpr double kLumaR = 0.2126;
constexpr double kLumaG = 0.7152;
constexpr double kLumaB = 0.0722;
constexpr double kEpsilon = 1.0e-9;

int safeTemporalWindow(const DeflickerParams &params)
{
    int window = qMax(1, params.temporalWindow);
    if ((window & 1) == 0)
        ++window;
    return window;
}

int safeBandHeight(const DeflickerParams &params)
{
    return qMax(1, params.bandHeight);
}

int bandCountForHeight(int height, int bandHeight)
{
    if (height <= 0 || bandHeight <= 0)
        return 0;
    return (height + bandHeight - 1) / bandHeight;
}

void gainBounds(const DeflickerParams &params, double &minGain, double &maxGain)
{
    minGain = std::isfinite(params.minGain) ? qMax(0.0, params.minGain) : 0.5;
    maxGain = std::isfinite(params.maxGain) ? qMax(0.0, params.maxGain) : 2.0;
    if (minGain > maxGain)
        std::swap(minGain, maxGain);
}

double clampedStrength(const DeflickerParams &params)
{
    return qBound(0.0, std::isfinite(params.strength) ? params.strength : 0.0, 1.0);
}

double targetForSeries(const QVector<double> &series,
                      int frameIndex,
                      const DeflickerParams &params)
{
    const int seriesSize = static_cast<int>(series.size());
    if (seriesSize <= 0 || frameIndex < 0 || frameIndex >= seriesSize)
        return 0.0;

    const int halfWindow = safeTemporalWindow(params) / 2;
    const int first = qMax(0, frameIndex - halfWindow);
    const int last = qMin(seriesSize - 1, frameIndex + halfWindow);
    QVector<double> values;
    values.reserve(last - first + 1);
    for (int i = first; i <= last; ++i) {
        if (std::isfinite(series.at(i)))
            values.append(series.at(i));
    }
    if (values.isEmpty())
        return 0.0;

    if (!params.useMedianTarget) {
        double sum = 0.0;
        for (const double value : values)
            sum += value;
        return sum / static_cast<double>(values.size());
    }

    std::sort(values.begin(), values.end());
    const int valueCount = static_cast<int>(values.size());
    const int middle = valueCount / 2;
    if ((valueCount & 1) != 0)
        return values.at(middle);
    return (values.at(middle - 1) + values.at(middle)) * 0.5;
}

double targetForBand(const QVector<FrameStats> &stats,
                     int frameIndex,
                     int bandIndex,
                     const DeflickerParams &params)
{
    const int statsSize = static_cast<int>(stats.size());
    if (statsSize <= 0 || frameIndex < 0 || frameIndex >= statsSize)
        return 0.0;

    const int halfWindow = safeTemporalWindow(params) / 2;
    const int first = qMax(0, frameIndex - halfWindow);
    const int last = qMin(statsSize - 1, frameIndex + halfWindow);
    QVector<double> values;
    values.reserve(last - first + 1);
    for (int i = first; i <= last; ++i) {
        const QVector<double> &bands = stats.at(i).bandLuma;
        if (bandIndex >= 0 && bandIndex < bands.size()
            && std::isfinite(bands.at(bandIndex))) {
            values.append(bands.at(bandIndex));
        }
    }
    if (values.isEmpty())
        return 0.0;

    if (!params.useMedianTarget) {
        double sum = 0.0;
        for (const double value : values)
            sum += value;
        return sum / static_cast<double>(values.size());
    }

    std::sort(values.begin(), values.end());
    const int valueCount = static_cast<int>(values.size());
    const int middle = valueCount / 2;
    if ((valueCount & 1) != 0)
        return values.at(middle);
    return (values.at(middle - 1) + values.at(middle)) * 0.5;
}

double appliedGain(double target, double measured, const DeflickerParams &params)
{
    double minGain = 0.0;
    double maxGain = 0.0;
    gainBounds(params, minGain, maxGain);

    double rawGain = 1.0;
    if (measured <= kEpsilon) {
        rawGain = target > kEpsilon ? maxGain : 1.0;
    } else if (target > kEpsilon) {
        rawGain = target / measured;
    }

    if (!std::isfinite(rawGain))
        rawGain = 1.0;
    const double blended = 1.0 + (rawGain - 1.0) * clampedStrength(params);
    return qBound(minGain, blended, maxGain);
}

bool isIdentityCorrection(const FrameCorrection &correction)
{
    if (correction.gainR != 1.0
        || correction.gainG != 1.0
        || correction.gainB != 1.0) {
        return false;
    }
    for (const double gain : correction.bandGain) {
        if (gain != 1.0)
            return false;
    }
    return true;
}

double rowGainAt(int y, int height, int bandHeight,
                 const QVector<double> &bandGain)
{
    if (height <= 0 || bandHeight <= 0 || bandGain.isEmpty())
        return 1.0;

    const int expectedBands = bandCountForHeight(height, bandHeight);
    const int count = qMin(expectedBands, static_cast<int>(bandGain.size()));
    if (count <= 0)
        return 1.0;
    if (count == 1)
        return bandGain.at(0);

    const auto centerForBand = [height, bandHeight](int band) {
        const int first = band * bandHeight;
        const int last = qMin(height - 1, first + bandHeight - 1);
        return (first + last) * 0.5;
    };

    const int currentBand = qBound(0, y / bandHeight, count - 1);
    const double currentCenter = centerForBand(currentBand);
    int leftBand = currentBand;
    int rightBand = currentBand;
    if (y <= currentCenter && currentBand > 0) {
        leftBand = currentBand - 1;
    } else if (y > currentCenter && currentBand + 1 < count) {
        rightBand = currentBand + 1;
    }
    if (leftBand == rightBand)
        return bandGain.at(currentBand);

    const double left = centerForBand(leftBand);
    const double right = centerForBand(rightBand);
    const double t = (right - left) <= kEpsilon
        ? 0.0 : qBound(0.0, (y - left) / (right - left), 1.0);
    return bandGain.at(leftBand)
        + (bandGain.at(rightBand) - bandGain.at(leftBand)) * t;
}

int roundedChannel(double value)
{
    return qBound(0, static_cast<int>(std::lround(value)), 255);
}

} // namespace

FrameStats analyzeFrame(const QImage &frame, const DeflickerParams &params)
{
    FrameStats stats;
    if (frame.isNull())
        return stats;

    const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
    const QRect region = effectiveAnalysisRegion(rgba, params);
    if (region.isEmpty())
        return stats;

    double rSum = 0.0;
    double gSum = 0.0;
    double bSum = 0.0;
    const int pixelCount = region.width() * region.height();
    for (int y = region.top(); y <= region.bottom(); ++y) {
        const uchar *row = rgba.constScanLine(y);
        for (int x = region.left(); x <= region.right(); ++x) {
            const uchar *pixel = row + x * 4;
            rSum += pixel[0];
            gSum += pixel[1];
            bSum += pixel[2];
        }
    }

    const double count = static_cast<double>(pixelCount);
    stats.rMean = rSum / count;
    stats.gMean = gSum / count;
    stats.bMean = bSum / count;
    stats.lumaMean = kLumaR * stats.rMean
        + kLumaG * stats.gMean
        + kLumaB * stats.bMean;

    const int bandHeight = safeBandHeight(params);
    const int bandCount = bandCountForHeight(rgba.height(), bandHeight);
    stats.bandLuma.resize(bandCount);
    for (int band = 0; band < bandCount; ++band) {
        const int firstY = qMax(region.top(), band * bandHeight);
        const int lastY = qMin(region.bottom(),
                               qMin(rgba.height() - 1,
                                    (band + 1) * bandHeight - 1));
        if (firstY > lastY) {
            stats.bandLuma[band] = std::numeric_limits<double>::quiet_NaN();
            continue;
        }

        double sum = 0.0;
        int samples = 0;
        for (int y = firstY; y <= lastY; ++y) {
            const uchar *row = rgba.constScanLine(y);
            for (int x = region.left(); x <= region.right(); ++x) {
                const uchar *pixel = row + x * 4;
                sum += kLumaR * pixel[0]
                    + kLumaG * pixel[1]
                    + kLumaB * pixel[2];
                ++samples;
            }
        }
        stats.bandLuma[band] = samples > 0
            ? sum / samples : std::numeric_limits<double>::quiet_NaN();
    }
    return stats;
}

FrameCorrection computeCorrection(const QVector<FrameStats> &stats,
                                  int frameIndex,
                                  const DeflickerParams &params)
{
    FrameCorrection correction;
    if (stats.isEmpty() || frameIndex < 0 || frameIndex >= stats.size())
        return correction;

    QVector<double> luma;
    QVector<double> red;
    QVector<double> green;
    QVector<double> blue;
    luma.reserve(stats.size());
    red.reserve(stats.size());
    green.reserve(stats.size());
    blue.reserve(stats.size());
    for (const FrameStats &frame : stats) {
        luma.append(frame.lumaMean);
        red.append(frame.rMean);
        green.append(frame.gMean);
        blue.append(frame.bMean);
    }

    switch (params.mode) {
    case Mode::GlobalRgb:
        correction.gainR = appliedGain(targetForSeries(red, frameIndex, params),
                                       stats.at(frameIndex).rMean, params);
        correction.gainG = appliedGain(targetForSeries(green, frameIndex, params),
                                       stats.at(frameIndex).gMean, params);
        correction.gainB = appliedGain(targetForSeries(blue, frameIndex, params),
                                       stats.at(frameIndex).bMean, params);
        break;
    case Mode::RollingBands: {
        const int bandCount = static_cast<int>(
            stats.at(frameIndex).bandLuma.size());
        if (bandCount <= 0)
            break;

        QVector<double> rawGains;
        QVector<bool> validBands;
        rawGains.resize(bandCount);
        validBands.resize(bandCount);
        for (int band = 0; band < bandCount; ++band) {
            const double measured = stats.at(frameIndex).bandLuma.at(band);
            validBands[band] = std::isfinite(measured);
            rawGains[band] = validBands.at(band)
                ? appliedGain(targetForBand(stats, frameIndex, band, params),
                              measured, params)
                : 1.0;
        }

        const double smoothing = qBound(
            0.0, std::isfinite(params.bandSmoothing) ? params.bandSmoothing : 0.0,
            1.0);
        correction.bandGain = rawGains;
        if (smoothing > 0.0 && bandCount > 1) {
            for (int band = 0; band < bandCount; ++band) {
                double neighborSum = 0.0;
                int neighborCount = 0;
                if (band > 0 && validBands.at(band - 1)) {
                    neighborSum += rawGains.at(band - 1);
                    ++neighborCount;
                }
                if (band + 1 < bandCount && validBands.at(band + 1)) {
                    neighborSum += rawGains.at(band + 1);
                    ++neighborCount;
                }
                if (!validBands.at(band))
                    continue;
                const double neighbors = neighborCount > 0
                    ? neighborSum / neighborCount : rawGains.at(band);
                correction.bandGain[band] = rawGains.at(band) * (1.0 - smoothing)
                    + neighbors * smoothing;
            }
        }
        break;
    }
    case Mode::GlobalLuma:
    default: {
        const double gain = appliedGain(targetForSeries(luma, frameIndex, params),
                                        stats.at(frameIndex).lumaMean, params);
        correction.gainR = gain;
        correction.gainG = gain;
        correction.gainB = gain;
        break;
    }
    }

    return correction;
}

QImage applyCorrection(const QImage &frame,
                       const FrameCorrection &correction,
                       const DeflickerParams &params)
{
    if (frame.isNull() || clampedStrength(params) <= 0.0
        || isIdentityCorrection(correction)) {
        return frame;
    }

    QImage output = frame.convertToFormat(QImage::Format_RGBA8888);
    const int bandHeight = safeBandHeight(params);
    for (int y = 0; y < output.height(); ++y) {
        const double rowGain = params.mode == Mode::RollingBands
            ? rowGainAt(y, output.height(), bandHeight, correction.bandGain)
            : 1.0;
        const double gainR = params.mode == Mode::RollingBands
            ? rowGain : correction.gainR;
        const double gainG = params.mode == Mode::RollingBands
            ? rowGain : correction.gainG;
        const double gainB = params.mode == Mode::RollingBands
            ? rowGain : correction.gainB;
        uchar *row = output.scanLine(y);
        for (int x = 0; x < output.width(); ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = static_cast<uchar>(roundedChannel(pixel[0] * gainR));
            pixel[1] = static_cast<uchar>(roundedChannel(pixel[1] * gainG));
            pixel[2] = static_cast<uchar>(roundedChannel(pixel[2] * gainB));
        }
    }
    return output;
}

StreamingProcessResult processSequenceStreaming(
    int frameCount,
    const FrameFetcher &frameFetcher,
    const DeflickerParams &params,
    const FrameSink &frameSink,
    const CancellationProbe &shouldCancel)
{
    StreamingProcessResult result;
    if (frameCount <= 0) {
        result.success = true;
        return result;
    }
    if (!frameFetcher) {
        result.error = QStringLiteral("フレーム取得経路が設定されていません。");
        return result;
    }

    result.stats.reserve(frameCount);
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        if (shouldCancel && shouldCancel()) {
            result.cancelled = true;
            return result;
        }

        const QImage frame = frameFetcher(frameIndex);
        if (shouldCancel && shouldCancel()) {
            result.cancelled = true;
            return result;
        }
        if (frame.isNull()) {
            result.error = QStringLiteral("フレーム %1 のデコードに失敗しました。")
                .arg(frameIndex);
            result.stats.clear();
            return result;
        }

        if (!params.analysisRegion.isEmpty()
            && effectiveAnalysisRegion(frame, params).isEmpty()) {
            result.error = QStringLiteral(
                "解析領域がフレーム範囲外です。キャンバスとフレームの解像度を確認してください。");
            result.stats.clear();
            return result;
        }
        result.stats.append(analyzeFrame(frame, params));
    }

    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        if (shouldCancel && shouldCancel()) {
            result.cancelled = true;
            return result;
        }

        const QImage frame = frameFetcher(frameIndex);
        if (shouldCancel && shouldCancel()) {
            result.cancelled = true;
            return result;
        }
        if (frame.isNull()) {
            result.error = QStringLiteral("フレーム %1 のデコードに失敗しました。")
                .arg(frameIndex);
            result.stats.clear();
            return result;
        }

        const QImage corrected = applyCorrection(
            frame, computeCorrection(result.stats, frameIndex, params), params);
        if (corrected.isNull()) {
            result.error = QStringLiteral("フレーム %1 の補正に失敗しました。")
                .arg(frameIndex);
            result.stats.clear();
            return result;
        }
        if (frameSink && !frameSink(frameIndex, corrected, &result.error)) {
            if (result.error.isEmpty())
                result.error = QStringLiteral("補正フレームの出力に失敗しました。");
            result.stats.clear();
            return result;
        }
        ++result.processedFrames;
        if (shouldCancel && shouldCancel()) {
            result.cancelled = true;
            return result;
        }
    }

    result.success = true;
    return result;
}

QVector<QImage> processSequence(const QVector<QImage> &frames,
                                const DeflickerParams &params)
{
    QVector<QImage> output;
    if (frames.isEmpty())
        return output;

    const int frameCount = static_cast<int>(frames.size());
    output.reserve(frameCount);
    const StreamingProcessResult result = processSequenceStreaming(
        frameCount,
        [&frames](int frameIndex) { return frames.at(frameIndex); },
        params,
        [&output](int, const QImage &frame, QString *) {
            output.append(frame);
            return true;
        });
    if (!result.success)
        output.clear();
    return output;
}

} // namespace deflicker
