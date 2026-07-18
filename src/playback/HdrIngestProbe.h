#pragma once

struct AVCodecParameters;

namespace hdringest {

struct ColorInputs {
    int primaries;
    int trc;
    int bitDepth;
    bool hasHdrMeta;
};

ColorInputs captureColorInputs(const AVCodecParameters* cp);

} // namespace hdringest
