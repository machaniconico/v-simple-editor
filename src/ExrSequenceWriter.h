#pragma once

#include <QImage>
#include <QString>
#include <QVector>
#include <optional>

namespace exrseq {

enum class PixelType { Half, Float };
enum class Compression { None, Rle, Zip };

struct Options {
    bool keepAlpha = false;
    PixelType pixelType = PixelType::Half;
    Compression compression = Compression::Zip;
    bool linearize = false;
};

bool encoderAvailable();
void setEncoderAvailableOverrideForTest(std::optional<bool> available);

// An empty string means success. The file is committed atomically only after
// the encoder has returned exactly one complete EXR packet.
QString encodeFrame(const QImage &image, const QString &path, const Options &options = {});

// Read EXR pixels as display-referred 8-bit data, with no transfer conversion.
bool decodeForTest(const QString &path, QImage *rgba8, bool *hasAlpha, QString *how);

// Read the decoder's planar float data directly; no QImage or gamma conversion.
// rgba contains width * height * 4 interleaved floats (opaque A when absent).
bool decodeFloatForTest(const QString &path, QVector<float> *rgba,
                        int *w, int *h, bool *hasAlpha);

// Parse the OpenEXR channels attribute, rather than inferring alpha from a
// decoder's output format. Used by the EXR writer's selftest.
bool headerHasAlphaForTest(const QString &path, bool *hasAlpha);

} // namespace exrseq
