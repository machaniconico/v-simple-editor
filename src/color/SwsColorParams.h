#pragma once

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace swscolor {

struct SdrTags {
    AVColorPrimaries pri;
    AVColorTransferCharacteristic trc;
    AVColorSpace spc;
    AVColorRange range;
};

AVColorSpace resolveColorspace(AVColorSpace tagged, int width, int height);
AVColorRange resolveRange(AVColorRange tagged);
int swsCoeffsId(AVColorSpace cs);
SdrTags sdrTagsFor(int width, int height);

} // namespace swscolor
