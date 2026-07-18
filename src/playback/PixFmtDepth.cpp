#include "PixFmtDepth.h"

#include <algorithm>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace pixfmtdepth {

int bitDepthFromPixFmt(int avPixFmt)
{
    const AVPixFmtDescriptor* d =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(avPixFmt));
    if (!d || d->nb_components <= 0)
        return 8;
#ifdef AV_PIX_FMT_FLAG_BAYER
    if (d->flags & AV_PIX_FMT_FLAG_BAYER)
        return std::max(8, av_get_bits_per_pixel(d));
#endif
    return std::max(8, static_cast<int>(d->comp[0].depth));
}

} // namespace pixfmtdepth
