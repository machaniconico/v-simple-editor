#include "HdrIngestProbe.h"

#include "PixFmtDepth.h"

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavcodec/version.h>
#include <libavutil/version.h>
}

namespace hdringest {

ColorInputs captureColorInputs(const AVCodecParameters* cp)
{
    ColorInputs inputs;
    inputs.primaries = cp->color_primaries;
    inputs.trc = cp->color_trc;
    inputs.bitDepth = pixfmtdepth::bitDepthFromPixFmt(cp->format);
    inputs.hasHdrMeta = false;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 0, 0)
    inputs.hasHdrMeta =
        av_packet_side_data_get(cp->coded_side_data,
                                cp->nb_coded_side_data,
                                AV_PKT_DATA_MASTERING_DISPLAY_METADATA) != nullptr
        || av_packet_side_data_get(cp->coded_side_data,
                                   cp->nb_coded_side_data,
                                   AV_PKT_DATA_CONTENT_LIGHT_LEVEL) != nullptr;
#endif
    return inputs;
}

} // namespace hdringest
