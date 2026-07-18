#include "FrameDiff.h"

namespace framediff {

double mse(const QImage &a, const QImage &b)
{
    if (a.width() != b.width() || a.height() != b.height())
        return -1.0;

    const QImage ia = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage ib = b.convertToFormat(QImage::Format_RGBA8888);

    const int w = ia.width();
    const int h = ia.height();
    if (w <= 0 || h <= 0)
        return 0.0;

    double acc = 0.0;
    for (int y = 0; y < h; ++y) {
        const uchar *ra = reinterpret_cast<const uchar *>(ia.constBits()) +
                          static_cast<qsizetype>(y) * ia.bytesPerLine();
        const uchar *rb = reinterpret_cast<const uchar *>(ib.constBits()) +
                          static_cast<qsizetype>(y) * ib.bytesPerLine();
        for (int x = 0; x < w * 4; ++x) {
            const double d = static_cast<double>(ra[x]) - static_cast<double>(rb[x]);
            acc += d * d;
        }
    }

    return acc / (static_cast<double>(w) * static_cast<double>(h) * 4.0);
}

double psnr(const QImage &a, const QImage &b)
{
    const double m = mse(a, b);
    if (m < 0.0)
        return -1.0;
    if (m == 0.0)
        return 1e9;
    return 10.0 * std::log10(255.0 * 255.0 / m);
}

} // namespace framediff
