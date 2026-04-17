#pragma once

#include <QImage>
#include <QString>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
}

// Stage 2 PiP — a lightweight software-only FFmpeg decoder used for the
// second video layer (V2+). Primary V1 playback stays on VideoPlayer's
// existing HW-accelerated path; SecondaryLayer is intentionally minimal so
// its lifecycle never interferes with the A/V-master-clock video entry.
//
// Design constraints:
//   * SW decode only (AV_HWDEVICE_TYPE_D3D11VA is environment-dependent
//     and double-allocating causes failures on some drivers).
//   * Single video stream per open file; audio streams are ignored here —
//     audio always comes from Timeline::computeAudioPlaybackSequence.
//   * No driver-side frame cache. The caller decides when to decode the
//     next frame; the last decoded QImage is cached for re-presentation
//     during pause.
class SecondaryLayer
{
public:
    SecondaryLayer();
    ~SecondaryLayer();

    // Open a new file. If another file was open, it is closed first.
    // Returns false on failure (file missing, unsupported codec, etc.).
    //
    // Stage 2.7 — optionally share a D3D11VA (or other HW) device context
    // from VideoPlayer so the secondary layer also gets HW decode. The
    // context is av_buffer_ref()ed internally; the caller retains ownership
    // of its own ref. Passing nullptr keeps the original SW-only behaviour.
    bool open(const QString &filePath,
              AVBufferRef *sharedHwDeviceCtx = nullptr);
    void close();

    bool isOpen() const { return m_formatCtx != nullptr && m_codecCtx != nullptr; }
    QString currentFilePath() const { return m_filePath; }

    // Seek to roughly fileLocalUs (in AV_TIME_BASE units). `precise` triggers
    // a frame-accurate decode loop; false uses the closest I-frame for
    // scrub-style seeks.
    bool seekToFileUs(int64_t fileLocalUs, bool precise);

    // Decode the next frame into a QImage (RGBA8888). Returns a null QImage
    // on EOF or error. ptsUsOut, if non-null, receives the frame PTS in
    // microseconds (file-local timebase).
    QImage decodeNextFrame(int64_t *ptsUsOut = nullptr);

    // Cached last successfully decoded frame. Useful when the caller wants
    // to re-present the same image without decoding (e.g. while paused on
    // the same frame and the primary layer advanced).
    const QImage &lastImage() const { return m_lastImage; }
    int64_t       lastPtsUs() const { return m_lastPtsUs; }

    int  videoStreamIndex() const { return m_videoStreamIndex; }
    int  width()  const;
    int  height() const;
    double aspectRatio() const;

private:
    bool openCodec(AVBufferRef *sharedHwDeviceCtx);
    QImage frameToRgba(AVFrame *frame);
    int64_t ptsToUs(int64_t pts) const;
    int64_t usToPts(int64_t us)  const;

    // Stage 2.7 — get_format callback invoked by libavcodec once the HW
    // pixel format is known. Looks at ctx->opaque to find the owning
    // SecondaryLayer so m_hwPixFmt can be recorded for later transfer.
    static enum AVPixelFormat getHwFormatCallback(AVCodecContext *ctx,
                                                   const enum AVPixelFormat *pixFmts);

    AVFormatContext *m_formatCtx     = nullptr;
    AVCodecContext  *m_codecCtx      = nullptr;
    SwsContext      *m_swsCtx        = nullptr;
    AVFrame         *m_frame         = nullptr;
    AVFrame         *m_swFrame       = nullptr;    // receives HW→SW transfer
    AVPacket        *m_packet        = nullptr;
    AVBufferRef     *m_hwDeviceCtx   = nullptr;    // our av_buffer_ref copy
    AVPixelFormat    m_hwPixFmt      = AV_PIX_FMT_NONE;
    int              m_videoStreamIndex = -1;
    int              m_swsWidth      = 0;
    int              m_swsHeight     = 0;
    AVPixelFormat    m_swsSrcFormat  = AV_PIX_FMT_NONE;

    QString m_filePath;
    QImage  m_lastImage;
    int64_t m_lastPtsUs = 0;
};
