#include "../Deflicker.h"
#include "../DeflickerDialog.h"

#include <QApplication>
#include <QColor>
#include <QCloseEvent>
#include <QImage>
#include <QDebug>
#include <QKeyEvent>
#include <QMetaObject>
#include <QTemporaryDir>

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

void printGate(int gate, const char *name, bool ok, const QString &detail,
               int &passed, int &failed)
{
    if (ok) {
        ++passed;
        qInfo().noquote() << QStringLiteral("[deflicker] PASS G%1 %2")
            .arg(gate).arg(QString::fromLatin1(name));
    } else {
        ++failed;
        qCritical().noquote() << QStringLiteral("[deflicker] FAIL G%1 %2: %3")
            .arg(gate).arg(QString::fromLatin1(name)).arg(detail);
    }
}

QImage makeSolidFrame(int width, int height, int r, int g, int b)
{
    QImage image(width, height, QImage::Format_RGB888);
    image.fill(QColor(r, g, b));
    return image;
}

QVector<QImage> makeLumaFlicker(int count, int width, int height)
{
    QVector<QImage> frames;
    frames.reserve(count);
    for (int i = 0; i < count; ++i) {
        const double gain = 1.0 + 0.25 * std::sin(kTwoPi * i / 4.0);
        const int value = qBound(0, static_cast<int>(std::lround(128.0 * gain)), 255);
        frames.append(makeSolidFrame(width, height, value, value, value));
    }
    return frames;
}

QVector<QImage> makeRedFlicker(int count, int width, int height)
{
    QVector<QImage> frames;
    frames.reserve(count);
    for (int i = 0; i < count; ++i) {
        const double gain = 1.0 + 0.25 * std::sin(kTwoPi * i / 4.0);
        const int red = qBound(0, static_cast<int>(std::lround(120.0 * gain)), 255);
        frames.append(makeSolidFrame(width, height, red, 90, 70));
    }
    return frames;
}

QVector<QImage> makeRollingFlicker(int count, int width, int height)
{
    QVector<QImage> frames;
    frames.reserve(count);
    for (int i = 0; i < count; ++i) {
        const double gain = 1.0 + 0.25 * std::sin(kTwoPi * i / 4.0);
        const int top = qBound(0, static_cast<int>(std::lround(128.0 * gain)), 255);
        QImage image(width, height, QImage::Format_RGB888);
        for (int y = 0; y < height; ++y) {
            const int value = y < height / 2 ? top : 128;
            for (int x = 0; x < width; ++x)
                image.setPixelColor(x, y, QColor(value, value, value));
        }
        frames.append(image);
    }
    return frames;
}

double meanLuma(const QImage &image, const QRect &area = QRect())
{
    if (image.isNull())
        return 0.0;
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QRect region = area.isEmpty() ? rgba.rect() : area.intersected(rgba.rect());
    if (region.isEmpty())
        region = rgba.rect();

    double sum = 0.0;
    int count = 0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        const uchar *row = rgba.constScanLine(y);
        for (int x = region.left(); x <= region.right(); ++x) {
            const uchar *pixel = row + x * 4;
            sum += 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2];
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

double variance(const QVector<double> &values)
{
    if (values.isEmpty())
        return 0.0;
    double mean = 0.0;
    for (const double value : values)
        mean += value;
    mean /= values.size();

    double sum = 0.0;
    for (const double value : values) {
        const double delta = value - mean;
        sum += delta * delta;
    }
    return sum / values.size();
}

double standardDeviation(const QVector<double> &values)
{
    return std::sqrt(variance(values));
}

int maxPixelDifference(const QImage &a, const QImage &b,
                       const QRect &area = QRect())
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.size() != bb.size())
        return 255;
    QRect region = area.isEmpty() ? aa.rect() : area.intersected(aa.rect());
    if (region.isEmpty())
        return 0;

    int maximum = 0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        const uchar *arow = aa.constScanLine(y);
        const uchar *brow = bb.constScanLine(y);
        for (int x = region.left(); x <= region.right(); ++x) {
            const uchar *ap = arow + x * 4;
            const uchar *bp = brow + x * 4;
            maximum = qMax(maximum, std::abs(int(ap[0]) - int(bp[0])));
            maximum = qMax(maximum, std::abs(int(ap[1]) - int(bp[1])));
            maximum = qMax(maximum, std::abs(int(ap[2]) - int(bp[2])));
        }
    }
    return maximum;
}

int maxGreenBlueDifference(const QImage &a, const QImage &b)
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.size() != bb.size())
        return 255;

    int maximum = 0;
    for (int y = 0; y < aa.height(); ++y) {
        const uchar *arow = aa.constScanLine(y);
        const uchar *brow = bb.constScanLine(y);
        for (int x = 0; x < aa.width(); ++x) {
            const uchar *ap = arow + x * 4;
            const uchar *bp = brow + x * 4;
            maximum = qMax(maximum, std::abs(int(ap[1]) - int(bp[1])));
            maximum = qMax(maximum, std::abs(int(ap[2]) - int(bp[2])));
        }
    }
    return maximum;
}

bool equalRgbaBytes(const QImage &a, const QImage &b)
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.size() != bb.size())
        return false;
    for (int y = 0; y < aa.height(); ++y) {
        if (std::memcmp(aa.constScanLine(y), bb.constScanLine(y),
                        static_cast<std::size_t>(aa.width() * 4)) != 0) {
            return false;
        }
    }
    return true;
}

QVector<double> sequenceMeans(const QVector<QImage> &frames,
                              const QRect &area = QRect())
{
    QVector<double> values;
    values.reserve(frames.size());
    for (const QImage &frame : frames)
        values.append(meanLuma(frame, area));
    return values;
}

double maxAdjacentRowMeanDifference(const QImage &image)
{
    double maximum = 0.0;
    for (int y = 1; y < image.height(); ++y) {
        const double delta = std::abs(meanLuma(image, QRect(0, y - 1,
                                                              image.width(), 1))
                                      - meanLuma(image, QRect(0, y,
                                                              image.width(), 1)));
        maximum = qMax(maximum, delta);
    }
    return maximum;
}

struct ImageLifetimeTracker {
    int live = 0;
    int peak = 0;
};

struct TrackedImageStorage {
    ImageLifetimeTracker *tracker = nullptr;
    uchar *data = nullptr;
};

void releaseTrackedImage(void *info)
{
    auto *storage = static_cast<TrackedImageStorage *>(info);
    if (!storage)
        return;
    if (storage->tracker)
        --storage->tracker->live;
    delete[] storage->data;
    delete storage;
}

QImage makeTrackedFrame(int width, int height, ImageLifetimeTracker *tracker)
{
    const int bytesPerLine = width * 4;
    auto *storage = new TrackedImageStorage;
    storage->tracker = tracker;
    storage->data = new uchar[static_cast<std::size_t>(bytesPerLine * height)];
    std::memset(storage->data, 96,
                static_cast<std::size_t>(bytesPerLine * height));
    if (tracker) {
        ++tracker->live;
        tracker->peak = qMax(tracker->peak, tracker->live);
    }
    return QImage(storage->data, width, height, bytesPerLine,
                  QImage::Format_RGBA8888, releaseTrackedImage, storage);
}

bool runBusyDialogCancellationScenario(bool escape)
{
    if (!QApplication::instance())
        return false;

    QTemporaryDir tempDir;
    if (!tempDir.isValid())
        return false;

    DeflickerDialog dialog;
    bool eventAccepted = true;
    bool eventSent = false;
    int importerCalls = 0;
    const auto requestWindowClose = [&]() {
        if (eventSent)
            return;
        eventSent = true;
        if (escape) {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&dialog, &event);
            eventAccepted = event.isAccepted();
        } else {
            QCloseEvent event;
            QApplication::sendEvent(&dialog, &event);
            eventAccepted = event.isAccepted();
        }
    };

    deflicker::DeflickerDialogContext context;
    context.clipLabel = QStringLiteral("cancel-regression");
    context.sourcePath = tempDir.filePath(QStringLiteral("clip.mov"));
    context.frameCount = 2;
    context.fps = 24.0;
    context.frameFetcher = [&](int) {
        requestWindowClose();
        return makeSolidFrame(8, 6, 96, 96, 96);
    };
    context.sourceFrameFetcher = context.frameFetcher;
    context.sequenceImporter = [&](const QStringList &, double, QString *) {
        ++importerCalls;
        return true;
    };
    dialog.setContext(context);
    dialog.show();
    QApplication::processEvents();
    QMetaObject::invokeMethod(&dialog, "onApplyClicked", Qt::DirectConnection);
    const bool remainsVisible = dialog.isVisible();
    dialog.hide();

    return !eventAccepted && importerCalls == 0 && remainsVisible;
}

} // namespace

int runDeflickerSelftest()
{
    int passed = 0;
    int failed = 0;

    const QVector<QImage> lumaInput = makeLumaFlicker(30, 32, 24);
    deflicker::DeflickerParams lumaParams;
    lumaParams.temporalWindow = 9;
    const QVector<QImage> lumaOutput = deflicker::processSequence(lumaInput, lumaParams);
    const double lumaBefore = standardDeviation(sequenceMeans(lumaInput));
    const double lumaAfter = standardDeviation(sequenceMeans(lumaOutput));
    printGate(1, "synthetic luma flicker removal",
              lumaBefore > 0.0 && lumaAfter < lumaBefore * 0.10,
              QStringLiteral("std before=%1 after=%2")
                  .arg(lumaBefore, 0, 'f', 6).arg(lumaAfter, 0, 'f', 6),
              passed, failed);

    const QVector<QImage> constantInput(30, makeSolidFrame(32, 24, 128, 128, 128));
    const QVector<QImage> constantOutput = deflicker::processSequence(
        constantInput, lumaParams);
    int constantMaximum = 0;
    for (int i = 0; i < constantInput.size(); ++i)
        constantMaximum = qMax(constantMaximum,
                               maxPixelDifference(constantInput.at(i),
                                                  constantOutput.at(i)));
    printGate(2, "no-flicker input unchanged", constantMaximum <= 1,
              QStringLiteral("max pixel difference=%1").arg(constantMaximum),
              passed, failed);

    deflicker::DeflickerParams noCorrectionParams = lumaParams;
    noCorrectionParams.strength = 0.0;
    const QVector<QImage> noCorrection = deflicker::processSequence(
        lumaInput, noCorrectionParams);
    bool strengthZeroExact = noCorrection.size() == lumaInput.size();
    for (int i = 0; strengthZeroExact && i < lumaInput.size(); ++i)
        strengthZeroExact = equalRgbaBytes(lumaInput.at(i), noCorrection.at(i));
    printGate(3, "strength zero is bit exact", strengthZeroExact,
              QStringLiteral("frames=%1").arg(noCorrection.size()), passed, failed);

    const QVector<QImage> rgbInput = makeRedFlicker(30, 32, 24);
    deflicker::DeflickerParams rgbParams = lumaParams;
    rgbParams.mode = deflicker::Mode::GlobalRgb;
    const QVector<QImage> rgbOutput = deflicker::processSequence(rgbInput, rgbParams);
    QVector<double> redBefore;
    QVector<double> redAfter;
    for (int i = 0; i < rgbInput.size(); ++i) {
        const QImage before = rgbInput.at(i).convertToFormat(QImage::Format_RGBA8888);
        const QImage after = rgbOutput.at(i).convertToFormat(QImage::Format_RGBA8888);
        redBefore.append(before.constScanLine(0)[0]);
        redAfter.append(after.constScanLine(0)[0]);
    }
    int greenBlueDifference = 0;
    for (int i = 0; i < rgbInput.size(); ++i)
        greenBlueDifference = qMax(greenBlueDifference,
                                   maxGreenBlueDifference(rgbInput.at(i),
                                                          rgbOutput.at(i)));
    printGate(4, "global RGB correction",
              variance(redAfter) < variance(redBefore) * 0.10
                  && greenBlueDifference <= 1,
              QStringLiteral("red variance before=%1 after=%2, max RGB diff=%3")
                  .arg(variance(redBefore), 0, 'f', 6)
                  .arg(variance(redAfter), 0, 'f', 6)
                  .arg(greenBlueDifference),
              passed, failed);

    const QVector<QImage> rollingInput = makeRollingFlicker(30, 32, 32);
    deflicker::DeflickerParams rollingParams = lumaParams;
    rollingParams.mode = deflicker::Mode::RollingBands;
    rollingParams.bandHeight = 1;
    rollingParams.bandSmoothing = 0.0;
    const QVector<QImage> rollingOutput = deflicker::processSequence(
        rollingInput, rollingParams);
    const QRect upper(0, 0, 32, 16);
    const QRect lower(0, 16, 32, 16);
    const QVector<double> rollingUpperBefore = sequenceMeans(rollingInput, upper);
    const QVector<double> rollingUpperAfter = sequenceMeans(rollingOutput, upper);
    int lowerDifference = 0;
    for (int i = 0; i < rollingInput.size(); ++i)
        lowerDifference = qMax(lowerDifference,
                               maxPixelDifference(rollingInput.at(i),
                                                  rollingOutput.at(i), lower));
    printGate(5, "rolling-band flicker removal",
              variance(rollingUpperAfter) < variance(rollingUpperBefore) * 0.10
                  && lowerDifference <= 1,
              QStringLiteral("upper variance before=%1 after=%2, lower max diff=%3")
                  .arg(variance(rollingUpperBefore), 0, 'f', 6)
                  .arg(variance(rollingUpperAfter), 0, 'f', 6)
                  .arg(lowerDifference),
              passed, failed);

    deflicker::DeflickerParams smoothRollingParams = rollingParams;
    smoothRollingParams.bandHeight = 8;
    smoothRollingParams.bandSmoothing = 0.5;
    const QVector<QImage> smoothRollingOutput = deflicker::processSequence(
        rollingInput, smoothRollingParams);
    const QImage rollingBeforeBoundary = rollingInput.at(1);
    const QImage rollingAfterBoundary = smoothRollingOutput.at(1);
    const double boundaryBefore = maxAdjacentRowMeanDifference(rollingBeforeBoundary);
    const double boundaryAfter = maxAdjacentRowMeanDifference(rollingAfterBoundary);
    printGate(6, "rolling-band boundary smoothing",
              boundaryAfter <= boundaryBefore + 1.0e-9,
              QStringLiteral("max row delta before=%1 after=%2")
                  .arg(boundaryBefore, 0, 'f', 6).arg(boundaryAfter, 0, 'f', 6),
              passed, failed);

    QVector<deflicker::FrameStats> clampStats;
    deflicker::FrameStats bright;
    bright.lumaMean = bright.rMean = bright.gMean = bright.bMean = 128.0;
    deflicker::FrameStats black;
    clampStats.append(bright);
    clampStats.append(black);
    deflicker::DeflickerParams clampParams = lumaParams;
    clampParams.temporalWindow = 3;
    clampParams.maxGain = 1.4;
    clampParams.minGain = 0.5;
    const deflicker::FrameCorrection clamped = deflicker::computeCorrection(
        clampStats, 1, clampParams);
    printGate(7, "gain clamp", clamped.gainR <= 1.4 + 1.0e-12
                  && clamped.gainG <= 1.4 + 1.0e-12
                  && clamped.gainB <= 1.4 + 1.0e-12,
              QStringLiteral("gains=%1,%2,%3")
                  .arg(clamped.gainR, 0, 'f', 6)
                  .arg(clamped.gainG, 0, 'f', 6)
                  .arg(clamped.gainB, 0, 'f', 6),
              passed, failed);

    QVector<QImage> endpointInput;
    for (const int value : {60, 180, 60, 180, 60, 180, 60})
        endpointInput.append(makeSolidFrame(16, 12, value, value, value));
    deflicker::DeflickerParams endpointParams = lumaParams;
    endpointParams.useMedianTarget = false;
    endpointParams.temporalWindow = 5;
    const QVector<QImage> endpointOutput = deflicker::processSequence(
        endpointInput, endpointParams);
    const bool endpointChanged = maxPixelDifference(endpointInput.first(),
                                                     endpointOutput.first()) > 0
        && maxPixelDifference(endpointInput.last(), endpointOutput.last()) > 0;
    printGate(8, "endpoint frames are processed", endpointChanged,
              QStringLiteral("first diff=%1, last diff=%2")
                  .arg(maxPixelDifference(endpointInput.first(), endpointOutput.first()))
                  .arg(maxPixelDifference(endpointInput.last(), endpointOutput.last())),
              passed, failed);

    const QVector<QImage> deterministicA = deflicker::processSequence(
        rollingInput, smoothRollingParams);
    const QVector<QImage> deterministicB = deflicker::processSequence(
        rollingInput, smoothRollingParams);
    bool deterministic = deterministicA.size() == deterministicB.size();
    for (int i = 0; deterministic && i < deterministicA.size(); ++i)
        deterministic = equalRgbaBytes(deterministicA.at(i), deterministicB.at(i));
    printGate(9, "deterministic output", deterministic,
              QStringLiteral("frames=%1").arg(deterministicA.size()), passed, failed);

    ImageLifetimeTracker lifetime;
    constexpr int kStreamingFrames = 64;
    int streamingFetched = 0;
    int streamingWritten = 0;
    deflicker::DeflickerParams streamingParams = lumaParams;
    const deflicker::StreamingProcessResult streamingResult =
        deflicker::processSequenceStreaming(
            kStreamingFrames,
            [&](int) {
                ++streamingFetched;
                return makeTrackedFrame(32, 24, &lifetime);
            },
            streamingParams,
            [&](int, const QImage &image, QString *) {
                if (image.size() == QSize(32, 24))
                    ++streamingWritten;
                return true;
            });
    printGate(10, "streaming processing bounds image lifetime",
              streamingResult.success
                  && streamingResult.processedFrames == kStreamingFrames
                  && streamingFetched == kStreamingFrames * 2
                  && streamingWritten == kStreamingFrames
                  && lifetime.live == 0 && lifetime.peak <= 2,
              QStringLiteral("success=%1 processed=%2 fetched=%3 written=%4 live=%5 peak=%6")
                  .arg(streamingResult.success)
                  .arg(streamingResult.processedFrames)
                  .arg(streamingFetched)
                  .arg(streamingWritten)
                  .arg(lifetime.live)
                  .arg(lifetime.peak),
              passed, failed);

    bool sourceResolution = false;
    if (QApplication::instance()) {
        QTemporaryDir tempDir;
        if (tempDir.isValid()) {
            DeflickerDialog dialog;
            deflicker::DeflickerDialogContext context;
            context.clipLabel = QStringLiteral("source-resolution-regression");
            context.sourcePath = tempDir.filePath(QStringLiteral("clip.mov"));
            context.frameCount = 2;
            context.fps = 24.0;
            context.frameFetcher = [](int) {
                return makeSolidFrame(4, 3, 64, 64, 64);
            };
            context.sourceFrameFetcher = [](int) {
                return makeSolidFrame(16, 12, 64, 64, 64);
            };
            QVector<QSize> importedSizes;
            context.sequenceImporter = [&](const QStringList &paths, double, QString *) {
                for (const QString &path : paths)
                    importedSizes.append(QImage(path).size());
                return true;
            };
            dialog.setContext(context);
            QMetaObject::invokeMethod(&dialog, "onApplyClicked", Qt::DirectConnection);
            sourceResolution = dialog.result() == QDialog::Accepted
                && importedSizes.size() == 2;
            for (const QSize &size : importedSizes)
                sourceResolution = sourceResolution && size == QSize(16, 12);
        }
    }
    printGate(11, "apply uses source-resolution frames",
              sourceResolution,
              QStringLiteral("source-resolution output gate=%1").arg(sourceResolution),
              passed, failed);

    const bool closeBlocked = runBusyDialogCancellationScenario(false);
    const bool escapeBlocked = runBusyDialogCancellationScenario(true);
    printGate(12, "busy dialog blocks close and escape before import",
              closeBlocked && escapeBlocked,
              QStringLiteral("close=%1 escape=%2").arg(closeBlocked).arg(escapeBlocked),
              passed, failed);

    const QRect canvasMask(480, 270, 960, 540);
    const QRect scaledMask = deflicker::scaleAnalysisRegion(
        canvasMask, QSize(1920, 1080), QSize(3840, 2160));
    printGate(13, "mask region scales from canvas to source frame",
              scaledMask == QRect(960, 540, 1920, 1080),
              QStringLiteral("scaled=%1,%2 %3x%4")
                  .arg(scaledMask.x()).arg(scaledMask.y())
                  .arg(scaledMask.width()).arg(scaledMask.height()),
              passed, failed);

    deflicker::DeflickerParams outsideParams = lumaParams;
    outsideParams.analysisRegion = QRect(100, 100, 8, 8);
    const deflicker::FrameStats outsideStats = deflicker::analyzeFrame(
        makeSolidFrame(16, 12, 120, 120, 120), outsideParams);
    const deflicker::StreamingProcessResult outsideResult =
        deflicker::processSequenceStreaming(
            1,
            [](int) { return makeSolidFrame(16, 12, 120, 120, 120); },
            outsideParams);
    printGate(14, "out-of-frame analysis region is rejected",
              outsideStats.lumaMean == 0.0
                  && outsideResult.error.contains(QStringLiteral("範囲")),
              QStringLiteral("luma=%1 error=%2")
                  .arg(outsideStats.lumaMean, 0, 'f', 3)
                  .arg(outsideResult.error),
              passed, failed);

    qInfo().noquote() << QStringLiteral("DEFLICKER-SELFTEST: %1/14 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : 1;
}
