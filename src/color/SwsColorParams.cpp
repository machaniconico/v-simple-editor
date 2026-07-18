#include "SwsColorParams.h"

namespace swscolor {

AVColorSpace resolveColorspace(AVColorSpace tagged, int, int height)
{
    if (tagged != AVCOL_SPC_UNSPECIFIED)
        return tagged;
    return height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
}

AVColorRange resolveRange(AVColorRange tagged)
{
    if (tagged != AVCOL_RANGE_UNSPECIFIED)
        return tagged;
    return AVCOL_RANGE_MPEG;
}

int swsCoeffsId(AVColorSpace cs)
{
    switch (cs) {
    case AVCOL_SPC_BT709:
        return SWS_CS_ITU709;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
        return SWS_CS_ITU601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return SWS_CS_BT2020;
    case AVCOL_SPC_SMPTE240M:
        return SWS_CS_SMPTE240M;
    case AVCOL_SPC_FCC:
        return SWS_CS_FCC;
    default:
        return SWS_CS_ITU709;
    }
}

SdrTags sdrTagsFor(int, int height)
{
    if (height >= 720) {
        return SdrTags{
            AVCOL_PRI_BT709,
            AVCOL_TRC_BT709,
            AVCOL_SPC_BT709,
            AVCOL_RANGE_MPEG
        };
    }
    return SdrTags{
        AVCOL_PRI_SMPTE170M,
        AVCOL_TRC_SMPTE170M,
        AVCOL_SPC_SMPTE170M,
        AVCOL_RANGE_MPEG
    };
}

} // namespace swscolor
