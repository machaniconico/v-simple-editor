#pragma once

// ===========================================================================
// libavcore::VideoFilterGraph - Qt-free in-process video filter runner.
//
// Shared helper for callers moving video filtering in-process. Applies one
// libavfilter video chain to the first video stream of one input file,
// re-encodes that stream, and optionally stream-copies all non-video streams.
// ===========================================================================

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace libavcore {

struct VideoFilterRequest {
    std::string inputPath;
    std::string outputPath;
    std::string filterDescription;
    std::string videoCodecName;
    int64_t videoBitrateBits = 0;
    bool copyAudio = true;
};

class VideoFilterGraph {
public:
    VideoFilterGraph();
    ~VideoFilterGraph();

    VideoFilterGraph(const VideoFilterGraph&) = delete;
    VideoFilterGraph& operator=(const VideoFilterGraph&) = delete;

    std::optional<std::string> run(
        const VideoFilterRequest& req,
        const std::function<void(int)>& progressCallback = {},
        const std::function<bool()>& cancelCheck = {});
};

} // namespace libavcore
