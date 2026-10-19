#include "../CodecDetector.h"
#include "../ExrSequenceWriter.h"
#include "SelftestGate.h"

#include <QFile>
#include <QFileInfo>
#include <QColor>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace {
QImage gradient()
{
    QImage image(64, 64, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            row[4 * x] = static_cast<uchar>(x * 4);
            row[4 * x + 1] = static_cast<uchar>(y * 4);
            row[4 * x + 2] = static_cast<uchar>((x + y) * 2);
            row[4 * x + 3] = 128;
        }
    }
    return image;
}

bool nearEightBit(const QImage &actual, const QImage &expected)
{
    if (actual.size() != expected.size() || actual.format() != QImage::Format_RGBA8888)
        return false;
    for (int y = 0; y < expected.height(); ++y) {
        const uchar *a = actual.constScanLine(y);
        const uchar *e = expected.constScanLine(y);
        for (int x = 0; x < expected.width(); ++x)
            for (int c = 0; c < 4; ++c)
                if (std::abs(int(a[4 * x + c]) - int(e[4 * x + c])) > 1)
                    return false;
    }
    return true;
}

bool nearFloat(const QVector<float> &actual, const QImage &expected, float tolerance)
{
    if (actual.size() != expected.width() * expected.height() * 4) return false;
    for (int y = 0; y < expected.height(); ++y) {
        const uchar *row = expected.constScanLine(y);
        for (int x = 0; x < expected.width(); ++x)
            for (int c = 0; c < 4; ++c)
                if (std::abs(actual[(y * expected.width() + x) * 4 + c]
                             - row[4 * x + c] / 255.0f) > tolerance)
                    return false;
    }
    return true;
}

QString pathIn(const QTemporaryDir &dir, const char *name)
{
    return dir.filePath(QString::fromLatin1(name));
}

void printStepZero()
{
    const AVCodec *codec = avcodec_find_encoder_by_name("exr");
    std::fprintf(stderr, "exr-writer Step 0: isEncoderAvailable(exr)=%s; pix_fmts=",
                 CodecDetector::isEncoderAvailable(QStringLiteral("exr")) ? "true" : "false");
    if (codec && codec->pix_fmts) {
        for (const AVPixelFormat *format = codec->pix_fmts; *format != AV_PIX_FMT_NONE; ++format)
            std::fprintf(stderr, "%s%s", format == codec->pix_fmts ? "" : ",",
                         av_get_pix_fmt_name(*format));
    } else {
        std::fprintf(stderr, "unavailable");
    }
    bool formatFound = false, compressionFound = false;
    if (codec) {
        AVCodecContext *context = avcodec_alloc_context3(codec);
        if (context && context->priv_data) {
            formatFound = av_opt_find(context->priv_data, "format", nullptr, 0, 0) != nullptr;
            compressionFound = av_opt_find(context->priv_data, "compression", nullptr, 0, 0) != nullptr;
        }
        avcodec_free_context(&context);
    }
    std::fprintf(stderr, "; av_opt_find(format)=%s; av_opt_find(compression)=%s\n",
                 formatFound ? "true" : "false", compressionFound ? "true" : "false");
}
} // namespace

int runExrWriterSelftest()
{
    selftest::GateTally tally;
    printStepZero();
    // The test harness may simulate an absent codec. Keep that environment
    // handling in the selftest; the production writer only consults its seam.
    if (!selftest::requireEncoder(QStringLiteral("exr")))
        exrseq::setEncoderAvailableOverrideForTest(false);
    if (!exrseq::encoderAvailable()) {
        for (int gate = 1; gate <= 5; ++gate)
            tally.skip(gate, "exrseq::encoderAvailable() is false");
        exrseq::setEncoderAvailableOverrideForTest(std::nullopt);
        return tally.finish("exr-writer");
    }

    QTemporaryDir dir;
    const QImage source = gradient();
    const QString halfPath = pathIn(dir, "half-alpha.exr");
    const QString opaquePath = pathIn(dir, "half-opaque.exr");
    const QString floatPath = pathIn(dir, "float-none.exr");
    const QString deniedPath = pathIn(dir, "denied.exr");
    const QString restoredPath = pathIn(dir, "restored.exr");
    const QString linearPath = pathIn(dir, "linear.exr");
    const QString displayPath = pathIn(dir, "display.exr");
    exrseq::Options half;
    half.keepAlpha = true;
    const QString halfError = dir.isValid()
        ? exrseq::encodeFrame(source, halfPath, half) : QStringLiteral("temporary directory unavailable");
    QImage decoded;
    bool alpha = false;
    QString how;
    const bool readHalf = halfError.isEmpty()
        && exrseq::decodeForTest(halfPath, &decoded, &alpha, &how);
    tally.gate(1, readHalf && alpha && nearEightBit(decoded, source),
               QStringLiteral("how=%1, writeError=%2, decoded=%3x%4")
                   .arg(how, halfError).arg(decoded.width()).arg(decoded.height()).toStdString());

    exrseq::Options opaque;
    const QString opaqueError = exrseq::encodeFrame(source, opaquePath, opaque);
    bool headerAlpha = false, headerOpaque = true;
    const bool parsedAlpha = exrseq::headerHasAlphaForTest(halfPath, &headerAlpha);
    const bool parsedOpaque = opaqueError.isEmpty()
        && exrseq::headerHasAlphaForTest(opaquePath, &headerOpaque);
    tally.gate(2, parsedAlpha && parsedOpaque && headerAlpha && !headerOpaque,
               QStringLiteral("chlist A: keep=%1 drop=%2, writeError=%3")
                   .arg(headerAlpha).arg(headerOpaque).arg(opaqueError).toStdString());

    exrseq::setEncoderAvailableOverrideForTest(false);
    const QString deniedError = exrseq::encodeFrame(source, deniedPath, half);
    const bool deniedAbsent = !QFile::exists(deniedPath);
    exrseq::setEncoderAvailableOverrideForTest(std::nullopt);
    const QString restoredError = exrseq::encodeFrame(source, restoredPath, half);
    QImage restored;
    bool restoredAlpha = false;
    QString restoredHow;
    const bool restoredRead = restoredError.isEmpty()
        && exrseq::decodeForTest(restoredPath, &restored, &restoredAlpha, &restoredHow);
    tally.gate(3, !deniedError.isEmpty() && deniedAbsent && restoredRead
                  && restoredAlpha && nearEightBit(restored, source),
               QStringLiteral("deniedError=%1 absent=%2 restoredError=%3 how=%4")
                   .arg(deniedError).arg(deniedAbsent).arg(restoredError, restoredHow).toStdString());

    exrseq::Options full = half;
    full.pixelType = exrseq::PixelType::Float;
    full.compression = exrseq::Compression::None;
    const QString fullError = exrseq::encodeFrame(source, floatPath, full);
    QVector<float> floatPixels, halfPixels;
    int fw = 0, fh = 0, hw = 0, hh = 0;
    bool floatAlpha = false, halfAlpha = false;
    const bool fullRead = fullError.isEmpty()
        && exrseq::decodeFloatForTest(floatPath, &floatPixels, &fw, &fh, &floatAlpha);
    const bool halfRead = exrseq::decodeFloatForTest(halfPath, &halfPixels, &hw, &hh, &halfAlpha);
    const qint64 fullSize = QFileInfo(floatPath).size();
    const qint64 halfSize = QFileInfo(halfPath).size();
    tally.gate(4, fullRead && halfRead && floatAlpha && halfAlpha
                  && fw == 64 && fh == 64 && hw == 64 && hh == 64
                  && nearFloat(floatPixels, source, 1.0f / 1024.0f)
                  && nearFloat(halfPixels, source, 1.0f / 1024.0f)
                  && fullSize > halfSize,
               QStringLiteral("float=%1 bytes half=%2 bytes, read=%3/%4, writeError=%5")
                   .arg(fullSize).arg(halfSize).arg(fullRead).arg(halfRead).arg(fullError).toStdString());

    QImage middle(64, 64, QImage::Format_RGBA8888);
    middle.fill(QColor(128, 128, 128, 255));
    exrseq::Options linear;
    linear.linearize = true;
    const QString linearError = exrseq::encodeFrame(middle, linearPath, linear);
    const QString displayError = exrseq::encodeFrame(middle, displayPath, opaque);
    QVector<float> linearPixels, displayPixels;
    int lw = 0, lh = 0, dw = 0, dh = 0;
    bool linearAlpha = true, displayAlpha = true;
    const bool linearRead = linearError.isEmpty()
        && exrseq::decodeFloatForTest(linearPath, &linearPixels, &lw, &lh, &linearAlpha);
    const bool displayRead = displayError.isEmpty()
        && exrseq::decodeFloatForTest(displayPath, &displayPixels, &dw, &dh, &displayAlpha);
    const float linearValue = linearPixels.isEmpty() ? -1.0f : linearPixels[0];
    const float displayValue = displayPixels.isEmpty() ? -1.0f : displayPixels[0];
    tally.gate(5, linearRead && displayRead && !linearAlpha && !displayAlpha
                  && lw == 64 && lh == 64 && dw == 64 && dh == 64
                  && std::abs(linearValue - 0.2158f) <= 1.0f / 1024.0f
                  && std::abs(displayValue - 128.0f / 255.0f) <= 1.0f / 1024.0f,
               QStringLiteral("linear=%1 display=%2, read=%3/%4, errors=%5 / %6")
                   .arg(linearValue).arg(displayValue).arg(linearRead).arg(displayRead)
                   .arg(linearError, displayError).toStdString());
    return tally.finish("exr-writer");
}
