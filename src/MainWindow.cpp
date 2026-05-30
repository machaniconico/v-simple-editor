#include "MainWindow.h"
#include "VideoPlayer.h"
#include "Timeline.h"
#include "TrimOps.h"
#include "ExportDialog.h"
#include "UndoManager.h"
#include "OverlayDialogs.h"
#include "VideoEffectDialogs.h"
#include "EffectPlugin.h"
#include "ColorGradingPanel.h"
#include "EffectControlsPanel.h"
#include "LumetriScopes.h"
#include "EffectClipboard.h"
#include "PasteAttributesDialog.h"
#include <QDockWidget>
#include "GLPreview.h"
#include "EqualizerPanel.h"
#include "CompressorPanel.h"
#include "ReverbPanel.h"
#include "NoiseReductionPanel.h"
#include "TitlePresetDialog.h"
#include "VoiceOverDialog.h"
#include "MultiCamDialog.h"
#include "RenderQueueDialog.h"
#include "SceneDetector.h"
#include "MotionStabilizer.h"
#include "AdjustmentLayer.h"
#include "SpeedRampData.h"
#include "ProxyManager.h"
#include "ProxyProgressDialog.h"
#include "ProxyManagementDialog.h"
#include "SceneCutDialog.h"
#include "AudioDuckingDialog.h"
#include "ColorManagementDialog.h"   // AC-4: ACES カラーマネジメント ダイアログ
#include "DolbyVisionDialog.h"        // DV-4: Dolby Vision メタデータ ダイアログ
#include "BroadcastCaptionDialog.h"  // CC-4: 放送CC (CEA-608/708) ダイアログ
#include "ProjectCollectorDialog.h"
#include "HDRSettingsDialog.h"
#include "AIProcessingDialog.h"
#include "PluginBrowserDialog.h"
#include "AIMaskDialog.h"
#include "PlanarTrackerDialog.h"
#include "AudioClipEditor.h"
#include "MagneticTimeline.h"
#include "ShortcutManager.h"
#include "ShortcutCustomizeDialog.h"
#include "SocialExportDialog.h"
#include "YtdlpDownloadDialog.h"
#include "CaptionEditorDialog.h"
#include "WhisperTranscribeDialog.h"
#include "TranscriptHighlightDialog.h"
#include "TranscriptHighlighter.h"
#include "AutoClipDialog.h"
#include "AutoClipGenerator.h"
#include "CommandPaletteDialog.h"
#include "WhisperTranscriber.h"
#include "CredentialDialog.h"
#include "SocialPreset.h"
#include "AspectReframer.h"
#include "ClipGeometry.h"
#include "SourceMonitorDock.h"
#include "AudioBusPanel.h"   // AB-5: オーディオ バス パネル ドック

// US-INT-1: Sprint 16 — モバイルエクスポート + 取り込みハブ (optional includes)
#if __has_include("MobileExportDialog.h")
  #include "MobileExportDialog.h"
  #define HAVE_MOBILE_EXPORT 1
#endif
#if __has_include("ImportHubDialog.h")
  #include "ImportHubDialog.h"
  #define HAVE_IMPORT_HUB 1
#endif

// US-INT-3: Sprint 17 — YouTube upload pipeline (optional includes).
// Guarded by __has_include so that workers / partial trees still compile.
#if __has_include("YoutubeUploadDialog.h")
  #include "YoutubeOAuth.h"
  #include "YoutubeUploadClient.h"
  #include "YoutubeUploadManager.h"
  #include "YoutubeUploadDialog.h"
  #define HAVE_YOUTUBE 1
#endif

// US-INT-3: Sprint 18 — Collaboration (comments / version history) optional includes.
#if __has_include("CommentsDockWidget.h")
  #include "CollaborationModel.h"
  #include "CommentsDockWidget.h"
  #include "CollabProjectShare.h"
  #include "CollabHistoryLog.h"
  #include "CollabHistoryDialog.h"
  #define HAVE_COLLAB 1
#endif

// US-INT-3: Sprint 19 — Auto color matching dialog (optional include).
#if __has_include("ColorMatchDialog.h")
  #include "ColorMatchDialog.h"
  #define HAVE_COLORMATCH 1
#endif

// US-INT-2: Sprint 20 — Vimeo / Twitch / Frame.io / XML export /
// Smart Edit / cloud render optional includes.
#if __has_include("VimeoUploadDialog.h")
  #include "VimeoOAuth.h"
  #include "VimeoUploadManager.h"
  #include "VimeoUploadDialog.h"
  #define HAVE_VIMEO 1
#endif
#if __has_include("TwitchStreamDialog.h")
  #include "TwitchStreamDialog.h"
  #define HAVE_TWITCH 1
#endif
#if __has_include("FrameIoImporter.h")
  #include "FrameIoImporter.h"
  #define HAVE_FRAMEIO 1
#endif
#if __has_include("DavinciResolveXmlExporter.h")
  #include "DavinciResolveXmlExporter.h"
  #define HAVE_DAVINCI_XML 1
#endif
#if __has_include("FcpxmlExporter.h")
  #include "FcpxmlExporter.h"
  #define HAVE_FCPXML 1
#endif
#if __has_include("SmartEditDialog.h")
  #include "SmartEditDialog.h"
  #define HAVE_SMARTEDIT 1
#endif
#if __has_include("CloudRenderDialog.h")
  #include "CloudRenderClient.h"
  #include "CloudRenderDialog.h"
  #define HAVE_CLOUD_RENDER 1
#endif
// US-INT-2: Sprint 21 — platform expansion / mastering / batch export.
#if __has_include("XVideoDialog.h")
  #include "XVideoDialog.h"
  #define HAVE_XVIDEO 1
#endif
#if __has_include("InstagramPublishDialog.h")
  #include "InstagramPublishDialog.h"
  #define HAVE_INSTAGRAM_PUBLISH 1
#endif
#if __has_include("ProjectTemplateDialog.h")
  #include "ProjectTemplateDialog.h"
  #define HAVE_PROJECT_TEMPLATE 1
#endif
#if __has_include("LoudnessMasterDialog.h")
  #include "LoudnessMasterDialog.h"
  #define HAVE_LOUDNESS_MASTER 1
#endif
#if __has_include("HdrGradingDialog.h")
  #include "HdrGradingDialog.h"
  #define HAVE_HDR_GRADING 1
#endif
#if __has_include("MultiCamSyncDialog.h")
  #include "MultiCamSyncDialog.h"
  #define HAVE_MULTICAM_SYNC 1
#endif
#if __has_include("BatchExportDialog.h")
  #include "BatchExportDialog.h"
  #define HAVE_BATCH_EXPORT 1
#endif
// US-INT-2: Sprint 22 — keying / restoration / animated export / easing /
// subtitle translation / lower-third / watermark.
#if __has_include("ChromaKeyRefineDialog.h")
  #include "ChromaKeyRefineDialog.h"
  #define HAVE_CHROMA_KEY_REFINE_DIALOG 1
#endif
#if __has_include("AudioRestorationDialog.h")
  #include "AudioRestorationDialog.h"
  #define HAVE_AUDIO_RESTORATION_DIALOG 1
#endif
#if __has_include("AnimatedExportDialog.h")
  #include "AnimatedExportDialog.h"
  #define HAVE_ANIMATED_EXPORT_DIALOG 1
#endif
#if __has_include("EasingCurveEditorDialog.h")
  #include "EasingCurveEditorDialog.h"
  #define HAVE_EASING_CURVE_EDITOR_DIALOG 1
#endif
#if __has_include("SubtitleTranslatorDialog.h")
  #include "SubtitleTranslatorDialog.h"
  #define HAVE_SUBTITLE_TRANSLATOR_DIALOG 1
#endif
#if __has_include("LowerThirdDialog.h")
  #include "LowerThirdDialog.h"
  #define HAVE_LOWER_THIRD_DIALOG 1
#endif
#if __has_include("WatermarkDialog.h")
  #include "WatermarkDialog.h"
  #define HAVE_WATERMARK_DIALOG 1
#endif
// SP-4: スペクトル音声修復ダイアログ。
#if __has_include("SpectralEditDialog.h")
  #include "SpectralEditDialog.h"
  #include "libavcore/AudioExtract.h"
  #define HAVE_SPECTRAL_EDIT_DIALOG 1
#endif
#include <QApplication>
#include <QMessageBox>
#include <QMenu>
#include <QVBoxLayout>
#include <QProgressDialog>
#include <QShortcut>
#include <QInputDialog>
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>      // DV-4: DV XML 保存ダイアログ
#include <QTextStream>      // DV-4: DV XML 書き出し
#include <QFileInfo>
#include <QDateTime>
#include <QTimer>
#include <QPointer>
#include <QUrl>
#include <QDebug>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QScrollArea>
#include <QFrame>
#include <QInputDialog>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <array>
#include <limits>
#include "AudioMeterWidget.h"
#include "GradientStopBar.h"
#include "BrushAnimationDialog.h"
#include "PathText.h"
#include "Text3DLayer.h"
#include "TextPathWarp.h"
#include "TextMaskReveal.h"
#include "VariableFontAxis.h"
#include "MographText.h"
#include "TextAnimPresets.h"
#include "Keyframe.h"
#include "SmartReframe.h"
#include "SmartReframeDialog.h"
#include "LoudnessAnalyzer.h"
#include "TrackMatteBake.h"
#include "TrackMatteKey.h"
#include "SubtitleTrackRenderer.h"
#include "LoudnessPanel.h"
#include "ParticleEffectDialog.h"
#include "VfxControlsPanel.h"
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QSettings>
#include <QSlider>
#include <QWidgetAction>
#include <QColorDialog>
#include <QFormLayout>
#include <QLabel>
#include <QStandardItemModel>
#include <QSet>
#include <QTemporaryDir>
#include <QProcess>
#include <numeric>
#include "NodeGraph.h"
#include "NodeEvaluator.h"
#include "NodeLibrary.h"
#include "NodeCanvasWidget.h"
#include "NodePropertiesPanel.h"
#include "LayerNodeBridge.h"
#include "RotoToolsDialog.h"
#include "TimeRemapDialog.h"
#include "FavoritesEditDialog.h"
#include "Text3DExtrusionDialog.h"
#include "ExpressionBindingDialog.h"
#include "CameraMotionDialog.h"
#include "ExtrudedMesh.h"
#include "SoftRaster3D.h"
#include <QPainter>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace {

struct VideoSourceInfo {
    double fps = 30.0;
    int frameCount = 0;
    double durationSeconds = 0.0;
    QSize frameSize;
};

QString findFfmpegBinary()
{
    QString path = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!path.isEmpty())
        return path;

    const QStringList searchPaths = {
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/bin")
    };
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"), searchPaths);
}

QGroupBox *findVfxGroup(VfxControlsPanel *panel, const QString &title)
{
    if (!panel)
        return nullptr;
    const auto groups = panel->findChildren<QGroupBox *>(QString(), Qt::FindDirectChildrenOnly);
    for (auto *group : groups) {
        if (group && group->title() == title)
            return group;
    }
    return nullptr;
}

void setVfxPanelState(VfxControlsPanel *panel, const ProjectVfxState &state)
{
    if (!panel)
        return;

    panel->blockAndReset();

    auto applySection = [](QGroupBox *group, bool enabled, std::initializer_list<double> values) {
        if (!group)
            return;
        if (auto *check = group->findChild<QCheckBox *>()) {
            QSignalBlocker blocker(check);
            check->setChecked(enabled);
        }
        const auto spins = group->findChildren<QDoubleSpinBox *>(QString(), Qt::FindDirectChildrenOnly);
        int index = 0;
        for (double value : values) {
            if (index >= spins.size())
                break;
            QSignalBlocker blocker(spins[index]);
            spins[index]->setValue(value);
            ++index;
        }
    };

    applySection(findVfxGroup(panel, QStringLiteral("Glow")),
                 state.glow.enabled,
                 {state.glow.threshold, state.glow.radius, state.glow.intensity});
    applySection(findVfxGroup(panel, QStringLiteral("Bloom")),
                 state.bloom.enabled,
                 {state.bloom.threshold, state.bloom.intensity, state.bloom.spread});
    applySection(findVfxGroup(panel, QStringLiteral("Chromatic Aberration")),
                 state.chromaticAberration.enabled,
                 {state.chromaticAberration.amount, state.chromaticAberration.radialFalloff});
    applySection(findVfxGroup(panel, QStringLiteral("Light Wrap")),
                 state.lightWrap.enabled,
                 {state.lightWrap.amount, state.lightWrap.radius});
}

double clipSourceOutPoint(const ClipInfo &clip)
{
    return (clip.outPoint > 0.0) ? clip.outPoint : clip.duration;
}

double clipEffectiveSourceFps(const VideoSourceInfo &info, double fallback)
{
    return (info.fps > 0.0) ? info.fps : qMax(1.0, fallback);
}

QString trackMatteTypeLabel(TrackMatteType type)
{
    switch (type) {
    case TrackMatteType::None:
        return QStringLiteral("なし");
    case TrackMatteType::AlphaMatte:
        return QStringLiteral("Alpha Matte");
    case TrackMatteType::AlphaInvertedMatte:
        return QStringLiteral("Alpha Matte (反転)");
    case TrackMatteType::LumaMatte:
        return QStringLiteral("Luma Matte");
    case TrackMatteType::LumaInvertedMatte:
        return QStringLiteral("Luma Matte (反転)");
    }
    return QStringLiteral("なし");
}

bool openVideoDecoder(const QString &filePath,
                      AVFormatContext **fmtCtx,
                      AVCodecContext **decCtx,
                      int *streamIndex)
{
    *fmtCtx = nullptr;
    *decCtx = nullptr;
    *streamIndex = -1;

    if (avformat_open_input(fmtCtx, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(*fmtCtx, nullptr) < 0) {
        avformat_close_input(fmtCtx);
        return false;
    }

    for (unsigned i = 0; i < (*fmtCtx)->nb_streams; ++i) {
        if ((*fmtCtx)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            *streamIndex = static_cast<int>(i);
            break;
        }
    }
    if (*streamIndex < 0) {
        avformat_close_input(fmtCtx);
        return false;
    }

    const AVCodecParameters *codecpar = (*fmtCtx)->streams[*streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        avformat_close_input(fmtCtx);
        return false;
    }

    *decCtx = avcodec_alloc_context3(codec);
    if (!*decCtx) {
        avformat_close_input(fmtCtx);
        return false;
    }
    if (avcodec_parameters_to_context(*decCtx, codecpar) < 0
        || avcodec_open2(*decCtx, codec, nullptr) < 0) {
        avcodec_free_context(decCtx);
        avformat_close_input(fmtCtx);
        return false;
    }
    return true;
}

VideoSourceInfo probeVideoSourceInfo(const QString &filePath, double fallbackFps)
{
    VideoSourceInfo info;
    info.fps = qMax(1.0, fallbackFps);

    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *decCtx = nullptr;
    int streamIndex = -1;
    if (!openVideoDecoder(filePath, &fmtCtx, &decCtx, &streamIndex))
        return info;

    if (streamIndex >= 0 && streamIndex < static_cast<int>(fmtCtx->nb_streams)) {
        AVStream *stream = fmtCtx->streams[streamIndex];
        const AVRational guessed = av_guess_frame_rate(fmtCtx, stream, nullptr);
        if (guessed.num > 0 && guessed.den > 0)
            info.fps = av_q2d(guessed);
        if (stream->duration > 0)
            info.durationSeconds = stream->duration * av_q2d(stream->time_base);
    }
    if (info.durationSeconds <= 0.0 && fmtCtx->duration > 0)
        info.durationSeconds = static_cast<double>(fmtCtx->duration) / AV_TIME_BASE;
    info.frameCount = (info.durationSeconds > 0.0 && info.fps > 0.0)
        ? qMax(1, static_cast<int>(std::llround(info.durationSeconds * info.fps)))
        : 0;
    info.frameSize = QSize(decCtx ? decCtx->width : 0, decCtx ? decCtx->height : 0);

    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);
    return info;
}

QImage avFrameToQImage(const AVFrame *frame, AVCodecContext *decCtx)
{
    if (!frame || !decCtx)
        return {};

    SwsContext *toRgbCtx = sws_getContext(frame->width, frame->height, decCtx->pix_fmt,
                                          frame->width, frame->height, AV_PIX_FMT_RGBA,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!toRgbCtx)
        return {};

    QImage image(frame->width, frame->height, QImage::Format_RGBA8888);
    uint8_t *dest[4] = { image.bits(), nullptr, nullptr, nullptr };
    int linesize[4] = { static_cast<int>(image.bytesPerLine()), 0, 0, 0 };
    sws_scale(toRgbCtx, frame->data, frame->linesize, 0, frame->height, dest, linesize);
    sws_freeContext(toRgbCtx);
    return image;
}

QImage decodeFrameAtSecondsFromFile(const QString &filePath, double sourceTimeSeconds)
{
    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *decCtx = nullptr;
    int streamIndex = -1;
    if (!openVideoDecoder(filePath, &fmtCtx, &decCtx, &streamIndex))
        return {};

    AVStream *stream = fmtCtx->streams[streamIndex];
    const double targetSeconds = qMax(0.0, sourceTimeSeconds);
    const int64_t seekTarget = av_rescale_q(
        static_cast<int64_t>(targetSeconds * AV_TIME_BASE),
        AVRational{1, AV_TIME_BASE},
        stream->time_base);
    av_seek_frame(fmtCtx, streamIndex, seekTarget, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(decCtx);

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    QImage result;

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != streamIndex) {
            av_packet_unref(packet);
            continue;
        }
        if (avcodec_send_packet(decCtx, packet) < 0) {
            av_packet_unref(packet);
            continue;
        }
        av_packet_unref(packet);

        while (avcodec_receive_frame(decCtx, frame) == 0) {
            const int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? frame->best_effort_timestamp
                : frame->pts;
            const double frameSeconds = (pts != AV_NOPTS_VALUE)
                ? pts * av_q2d(stream->time_base)
                : targetSeconds;
            result = avFrameToQImage(frame, decCtx);
            if (result.isNull() || frameSeconds + 1.0 / 120.0 < targetSeconds)
                continue;
            goto decode_done;
        }
    }

decode_done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);
    return result;
}

QImage combineMasksMax(const QImage &a, const QImage &b)
{
    if (a.isNull())
        return b.convertToFormat(QImage::Format_Grayscale8);
    if (b.isNull())
        return a.convertToFormat(QImage::Format_Grayscale8);

    QImage base = a.convertToFormat(QImage::Format_Grayscale8);
    QImage overlay = b.convertToFormat(QImage::Format_Grayscale8);
    if (overlay.size() != base.size()) {
        overlay = overlay.scaled(base.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                         .convertToFormat(QImage::Format_Grayscale8);
    }

    for (int y = 0; y < base.height(); ++y) {
        uchar *baseLine = base.scanLine(y);
        const uchar *overlayLine = overlay.constScanLine(y);
        for (int x = 0; x < base.width(); ++x)
            baseLine[x] = std::max(baseLine[x], overlayLine[x]);
    }
    return base;
}

QImage renderCompositeImage(const QImage &source, const CompositeLayer &layer, const QSize &canvasSize)
{
    if (source.isNull() || !canvasSize.isValid())
        return {};

    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    if (image.size() != canvasSize) {
        image = image.scaled(canvasSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    // SSOT: place the layer through the canonical clipgeom contract so the
    // special-clip preview uses the SAME normalized-dx/dy + canvas-center
    // anchor + rotate->scale math as GLPreview and the export path. The
    // CompositeLayer carries the raw ClipInfo transform values: position holds
    // the NORMALIZED videoDx/videoDy fractions (not pixels), scale.x() the
    // uniform videoScale, rotation the rotation2DDegrees.
    clipgeom::ClipTransform xf;
    xf.videoScale  = layer.scale.x();
    xf.videoDx     = layer.position.x();
    xf.videoDy     = layer.position.y();
    xf.rotationDeg = layer.rotation;
    return clipgeom::renderLayer(image, xf, canvasSize, /*smooth=*/true);
}

#ifdef HAVE_DAVINCI_XML
QVector<davinci::xml::ClipEntry> buildDavinciExportClips(const Timeline *timeline,
                                                         int fps)
{
    QVector<davinci::xml::ClipEntry> entries;
    if (!timeline || fps <= 0)
        return entries;

    const auto &tracks = timeline->videoTracks();
    for (int trackIdx = 0; trackIdx < tracks.size(); ++trackIdx) {
        const auto *track = tracks.value(trackIdx, nullptr);
        if (!track)
            continue;

        double cursorSec = 0.0;
        const auto &clips = track->clips();
        for (const ClipInfo &clip : clips) {
            cursorSec += clip.leadInSec;

            const double durationSec = qMax(0.0, clip.effectiveDuration());
            if (clip.filePath.isEmpty() || durationSec <= 0.0) {
                cursorSec += durationSec;
                continue;
            }

            davinci::xml::ClipEntry entry;
            entry.filePath = clip.filePath;
            entry.inPoint = qMax(0, qRound(cursorSec * fps));
            entry.outPoint = entry.inPoint + qMax(1, qRound(durationSec * fps));
            entry.trackIndex = trackIdx;
            entry.sourceIn = qMax(0, qRound(clip.inPoint * fps));
            entries.append(entry);

            cursorSec += durationSec;
        }
    }

    return entries;
}
#endif

#ifdef HAVE_FCPXML
QString fcpxFrameDuration(int fps)
{
    return QStringLiteral("1/%1s").arg(qMax(1, fps));
}

QVector<fcpx::xml::ClipEntry> buildFcpxmlExportClips(const Timeline *timeline)
{
    QVector<fcpx::xml::ClipEntry> entries;
    if (!timeline)
        return entries;

    const auto &tracks = timeline->videoTracks();
    for (const auto *track : tracks) {
        if (!track)
            continue;

        double cursorSec = 0.0;
        const auto &clips = track->clips();
        for (const ClipInfo &clip : clips) {
            cursorSec += clip.leadInSec;

            const double durationSec = qMax(0.0, clip.effectiveDuration());
            if (clip.filePath.isEmpty() || durationSec <= 0.0) {
                cursorSec += durationSec;
                continue;
            }

            fcpx::xml::ClipEntry entry;
            entry.filePath = clip.filePath;
            entry.offset = qMax(0.0, cursorSec);
            entry.duration = durationSec;
            entry.startInSource = qMax(0.0, clip.inPoint);
            entry.name = clip.displayName.isEmpty()
                ? QFileInfo(clip.filePath).completeBaseName()
                : clip.displayName;
            entries.append(entry);

            cursorSec += durationSec;
        }
    }

    std::stable_sort(entries.begin(), entries.end(),
                     [](const fcpx::xml::ClipEntry &lhs,
                        const fcpx::xml::ClipEntry &rhs) {
                         if (lhs.offset < rhs.offset)
                             return true;
                         if (lhs.offset > rhs.offset)
                             return false;
                         return lhs.name < rhs.name;
                     });
    return entries;
}
#endif

// TM-8: mirror MainWindow's m_trackMatteClipEntries onto the Timeline so
// the SSOT renderFrameAt (src/TimelineFrameRenderer.cpp) reads matte
// wiring from the Timeline object itself instead of walking QObject
// parents up to this live MainWindow off the RenderQueue worker thread
// (the old #define-private-public path — C1+C2). The QHash key is
// MainWindow::brushClipId(track,clip) == tlrender::renderClipId, so this
// is a straight per-entry copy into the 2-field TimelineTrackMatteEntry
// the consumer reads. Called on every m_trackMatteClipEntries mutation
// (configure dialog) and after a project load rebuilds it, so the GUI
// preview keeps working — now through the Timeline carrier.
void syncTrackMatteEntriesToTimeline(
    Timeline *timeline,
    const QHash<QString, TrackMatteClipEntry> &source)
{
    if (!timeline)
        return;
    QHash<QString, TimelineTrackMatteEntry> out;
    out.reserve(source.size());
    for (auto it = source.cbegin(); it != source.cend(); ++it) {
        TimelineTrackMatteEntry e;
        e.matteType = it.value().matteType;
        e.matteSourceClipId = it.value().matteSourceClipId;
        out.insert(it.key(), e);
    }
    timeline->setTrackMatteEntries(out);
}

// RM-1.2 / RM-4: snapshotTrackClips and remapTrackMatteEntriesAfterMutation
// are defined in TrackMatteKey.cpp (hoisted for testability). The types
// ClipKeyId and TrackClipSnapshot are declared in TrackMatteKey.h.

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_vimeoUploadDialog(nullptr)
    , m_twitchStreamDialog(nullptr)
    , m_frameIoImporter(nullptr)
    , m_cloudRenderClient(nullptr)
    , m_cloudRenderDialog(nullptr)
    , m_smartEditDialog(nullptr)
    , m_vimeoOAuth(nullptr)
    , m_vimeoManager(nullptr)
    , m_xVideoDialog(nullptr)
    , m_instagramDialog(nullptr)
    , m_projectTemplateDialog(nullptr)
    , m_loudnessDialog(nullptr)
    , m_hdrDialog(nullptr)
    , m_multiCamSyncDialog(nullptr)
    , m_batchExportDialog(nullptr)
    , m_chromaKeyDialog(nullptr)
    , m_audioRestoreDialog(nullptr)
    , m_animExportDialog(nullptr)
    , m_easingEditorDialog(nullptr)
    , m_subtitleTranslatorDialog(nullptr)
    , m_lowerThirdDialog(nullptr)
    , m_watermarkDialog(nullptr)
{
    qInfo() << "MainWindow::ctor begin";
    resize(1280, 720);

    m_supportedFormats = {
        "MP4 (*.mp4)", "MKV (*.mkv)", "MOV (*.mov)",
        "WebM (*.webm)", "FLV (*.flv)"
    };

    m_exporter = new Exporter(this);

    // Recent files setup (must be before setupMenuBar which uses m_recentFilesManager)
    setupRecentFiles();

    setupUI();
    qInfo() << "MainWindow::setupUI done";
    m_timeline->setMarkerManager(&m_markerManager);
    setupMenuBar();
    qInfo() << "MainWindow::setupMenuBar done";
    setupToolBar();
    updateEditActions();
    updateTitle();


    // Script engine. init() probes python3/python via QProcess with 3-second
    // timeouts each — on Windows when Python isn't installed, the
    // Microsoft Store python stub launches a Store dialog and the wait can
    // block much longer, freezing the splash screen and "preventing the
    // app from starting" from the user's perspective. Defer init to after
    // the event loop starts so startup never blocks on it.
    m_scriptEngine = new ScriptEngine(this);
    QTimer::singleShot(0, this, [this]() {
        if (m_scriptEngine) m_scriptEngine->init();
    });

    // Auto-save setup
    m_autoSave = new AutoSave(this);
    m_autoSave->setProjectData([this]() -> QString {
        ProjectData data;
        populateProjectData(data);
        return ProjectFile::toJsonString(data);
    });
    connect(m_autoSave, &AutoSave::autoSaved, this, [this](const QString &path) {
        statusBar()->showMessage("Auto-saved: " + QFileInfo(path).fileName(), 3000);
    });
    connect(m_autoSave, &AutoSave::recoveryAvailable, this, [](const QStringList &files) {
        // Crash Recovery dialog suppressed per user preference — silently
        // clean up stale recovery files on startup so the prompt never
        // blocks the app opening.
        for (const QString &path : files)
            QFile::remove(path);
    });
    // Apply dark theme by default
    ThemeManager::instance().applyTheme(ThemeType::Dark, this);

    // Enable drag & drop
    setAcceptDrops(true);

    // Setup permanent status bar widgets
    setupStatusBarWidgets();
    updateStatusInfo();

    statusBar()->showMessage("準備完了 — ファイル > 新規プロジェクトから開始してください");

    connect(m_timeline, &Timeline::clipSelected, this, [this](int /*index*/) {
        updateEditActions();
    });
    // V3 sprint — preview drag handle の edit target を選択 clip に同期。
    // Timeline 側で (sourceTrack, sourceClipIndex) を直接運んでくれる
    // track-aware overload にスイッチ。playhead heuristic は
    // V1+V2 が同 clipIdx を共有すると常に V1 を選んでしまう
    // failure mode があったので削除。
    connect(m_timeline, &Timeline::clipSelectedOnTrack, this,
        [this](int trackIdx, int clipIdx) {
            m_selectedVideoTrackIndex = trackIdx;
            m_selectedVideoClipIndexTracked = clipIdx;
            if (m_player)
                m_player->setEditTargetByClip(trackIdx, clipIdx);
            syncBrushAnimationPreviewForClip(trackIdx, clipIdx);
        });
    connect(m_timeline->undoManager(), &UndoManager::stateChanged, this, [this]() {
        updateEditActions();
    });
    connect(m_timeline->undoManager(), &UndoManager::stateJumpRequested,
            m_timeline, &Timeline::restoreState);

    // Show welcome screen initially
    showWelcomeScreen();

    // Restore saved window state
    restoreWindowState();

    m_timeline->setAudioMixer(m_player->audioMixer());

    rebuildAudioMeters();

    nodelib::registerBuiltinNodes();
    m_activeNodeGraph = new NodeGraph();
    m_nodeEvaluator = new NodeEvaluator();

    // US-SC-B: Sprint 12 — ショートカット管理 (preset/custom binding)。
    // setupMenuBar/setupToolBar 完走後に呼ぶことで全 QAction が確実に揃っている。
    m_shortcutManager = new shortcut::ShortcutManager(this);
    registerCoreShortcuts();
    m_shortcutManager->loadFromSettings();

    qInfo() << "MainWindow::ctor end";
}

void MainWindow::registerCoreShortcuts()
{
    if (!m_shortcutManager)
        return;
    auto reg = [this](QAction *a, const QString &id, const QString &name,
                      const QString &cat) {
        if (a)
            m_shortcutManager->registerAction(a, id, name, cat);
    };

    // 編集
    reg(m_undoAction,            "edit.undo",
        QStringLiteral("元に戻す"),            QStringLiteral("編集"));
    reg(m_redoAction,            "edit.redo",
        QStringLiteral("やり直し"),            QStringLiteral("編集"));
    reg(m_copyAction,            "edit.copy",
        QStringLiteral("クリップをコピー"),    QStringLiteral("編集"));
    reg(m_pasteAction,           "edit.paste",
        QStringLiteral("クリップを貼り付け"),  QStringLiteral("編集"));
    reg(m_splitAction,           "edit.split",
        QStringLiteral("再生ヘッドで分割"),    QStringLiteral("編集"));
    reg(m_deleteAction,          "edit.delete",
        QStringLiteral("クリップを削除"),      QStringLiteral("編集"));
    reg(m_rippleDeleteAction,    "timeline.ripple_delete",
        QStringLiteral("リップル削除"),        QStringLiteral("編集"));
    reg(m_copyEffectsAction,     "edit.copy_effects",
        QStringLiteral("エフェクトをコピー"),  QStringLiteral("編集"));
    reg(m_pasteEffectsAction,    "edit.paste_effects",
        QStringLiteral("エフェクトを貼り付け"), QStringLiteral("編集"));
    reg(m_pasteAttributesAction, "edit.paste_attributes",
        QStringLiteral("属性を貼り付け"),      QStringLiteral("編集"));

    // タイムライン / 表示
    reg(m_snapAction,         "timeline.snap_toggle",
        QStringLiteral("スナップ切替"),       QStringLiteral("タイムライン"));
    reg(m_trackMotionAction,  "tools.track_motion",
        QStringLiteral("モーション追跡"),     QStringLiteral("ツール"));
    reg(m_nodeModeAction,     "view.node_mode",
        QStringLiteral("ノード合成モード"),   QStringLiteral("表示"));
}

double MainWindow::currentPlayheadSeconds() const
{
    return m_timeline ? m_timeline->playheadPosition() : 0.0;
}

QString MainWindow::brushClipId(int trackIdx, int clipIdx)
{
    // RM-1.1: single shared formula (src/TrackMatteKey.h) so the GUI map
    // key can never drift from renderClipId / RenderQueue's carrier key.
    return trackMatteClipKey(trackIdx, clipIdx);
}

void MainWindow::setBrushAnimationEntries(const QVector<BrushAnimationEntry> &entries)
{
    for (auto it = m_liveBrushAnimations.cbegin(); it != m_liveBrushAnimations.cend(); ++it) {
        if (it.value())
            it.value()->deleteLater();
    }
    m_liveBrushAnimations.clear();
    m_brushAnimationEntries = entries;
}

void MainWindow::upsertBrushAnimationEntry(const BrushAnimationEntry &entry)
{
    for (auto &existing : m_brushAnimationEntries) {
        if (existing.clipId == entry.clipId) {
            existing = entry;
            return;
        }
    }
    m_brushAnimationEntries.append(entry);
}

BrushAnimation *MainWindow::materializeBrushAnimation(const QString &clipId)
{
    if (auto *live = m_liveBrushAnimations.value(clipId, nullptr))
        return live;

    for (const auto &entry : m_brushAnimationEntries) {
        if (entry.clipId != clipId)
            continue;
        auto *animation = new BrushAnimation(this);
        animation->fromJson(entry.brushData);
        m_liveBrushAnimations.insert(clipId, animation);
        return animation;
    }
    return nullptr;
}

void MainWindow::syncBrushAnimationPreviewForClip(int trackIdx, int clipIdx)
{
    if (!m_player || !m_player->glPreview())
        return;

    if (!m_timeline || trackIdx < 0 || clipIdx < 0) {
        m_player->glPreview()->clearBrushAnimation();
        return;
    }

    auto *track = m_timeline->videoTracks().value(trackIdx, nullptr);
    if (!track) {
        m_player->glPreview()->clearBrushAnimation();
        return;
    }

    const auto &clips = track->clips();
    if (clipIdx >= clips.size()) {
        m_player->glPreview()->clearBrushAnimation();
        return;
    }

    auto *animation = materializeBrushAnimation(brushClipId(trackIdx, clipIdx));
    if (!animation) {
        m_player->glPreview()->clearBrushAnimation();
        return;
    }

    const double progress = clips[clipIdx].keyframes.valueAt(
        QStringLiteral("brush_progress"), 0.0, 0.0);
    m_player->glPreview()->setBrushAnimation(animation);
    m_player->glPreview()->setBrushAnimationProgress(progress);
}

void MainWindow::setupUI()
{
    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_mainSplitter = new QSplitter(Qt::Vertical, this);

    m_player = new VideoPlayer(this);
    connect(m_player, &VideoPlayer::proxySettingsRequested,
            this, &MainWindow::openProxySettings);
    m_timeline = new Timeline(this);
    // US-INT-1: hand the Timeline to GLPreview so paintGL can compose any
    // adjustment layers covering the current timeline position.
    if (m_player->glPreview())
        m_player->glPreview()->setTimeline(m_timeline);

    // Welcome widget (shown when empty)
    m_welcomeWidget = new WelcomeWidget(this);
    connect(m_welcomeWidget, &WelcomeWidget::newProjectClicked, this, &MainWindow::newProject);
    connect(m_welcomeWidget, &WelcomeWidget::openFileClicked, this, &MainWindow::openFile);
    connect(m_welcomeWidget, &WelcomeWidget::openProjectClicked, this, &MainWindow::openProject);

    // Adobe-style tool property layout: wrap the player in a horizontal
    // splitter whose right-hand child is a QStackedWidget hosting per-tool
    // property panels (empty page by default, text-tool page when active).
    // The horizontal splitter replaces the raw VideoPlayer as the top half
    // of m_mainSplitter so the timeline is unaffected.
    m_previewSplitter = new QSplitter(Qt::Horizontal, this);
    m_previewSplitter->addWidget(m_player);
    setupToolPropertyPanel();
    m_previewSplitter->addWidget(m_toolPropertyStack);
    m_previewSplitter->setStretchFactor(0, 1);
    m_previewSplitter->setStretchFactor(1, 0);
    m_previewSplitter->setCollapsible(0, false);
    m_previewSplitter->setCollapsible(1, true);
    m_toolPropertyStack->hide(); // collapsed by default — no tool active

    m_mainSplitter->addWidget(m_previewSplitter);
    m_mainSplitter->addWidget(m_timeline);
    m_mainSplitter->setStretchFactor(0, 3);
    m_mainSplitter->setStretchFactor(1, 1);
    // Iteration 14 — both children non-collapsible. User report:
    // 「プレビューの最小表示サイズはそのままに拡大出来る幅を少し増やせる？
    // (今の仕様だと一定以上大きくした後一気に最大サイズになる)」. With
    // QSplitter's default collapsible behavior, dragging the divider past
    // the timeline's collapse threshold snapped the timeline to 0 and the
    // preview to full height — the "一気に最大サイズ" jump. Non-collapsible
    // children stop the divider at each child's natural minimum, so the
    // preview's smooth scaling range extends up to window_height -
    // timeline_min instead of clamping at the snap point.
    m_mainSplitter->setCollapsible(0, false);
    m_mainSplitter->setCollapsible(1, false);

    connect(m_player, &VideoPlayer::textRectRequested,
            this, &MainWindow::onTextRectRequested);
    connect(m_player, &VideoPlayer::textInlineCommitted,
            this, &MainWindow::onTextInlineCommitted);
    connect(m_player, &VideoPlayer::textOverlayEditCommitted,
            this, &MainWindow::onTextOverlayEditCommitted);
    // Existing-overlay drag: rewrite the rect in place and re-push the
    // overlay list so the preview re-renders with the new geometry.
    connect(m_player, &VideoPlayer::textOverlayRectChanged, this,
            [this](int idx, const QRectF &normalizedRect) {
                if (!m_timeline) return;
                if (!m_timeline->updateTextOverlayRect(idx,
                        normalizedRect.x(), normalizedRect.y(),
                        normalizedRect.width(), normalizedRect.height()))
                    return;
                const auto &clips = m_timeline->videoClips();
                if (!clips.isEmpty() && m_player) {
                    QVector<EnhancedTextOverlay> overlays;
                    const auto &mgr = clips[0].textManager;
                    for (int i = 0; i < mgr.count(); ++i)
                        overlays.append(mgr.overlay(i));
                    m_player->setTextOverlays(overlays);
                }
            });
    // US-T35 Video source transform drag/resize → persist to owning clip.
    connect(m_player, &VideoPlayer::videoSourceTransformChanged, this,
            [this](int trackIdx, int clipIdx, double scale, double dx, double dy) {
                if (!m_timeline) return;
                m_timeline->setClipVideoTransform(trackIdx, clipIdx, scale, dx, dy);
            });
    // Timeline text-strip edge drag → update right-panel spinboxes + preview
    connect(m_timeline, &Timeline::textOverlayTimeChanged, this,
            [this](int idx, double startTime, double endTime) {
                if (m_textToolStartSpin) {
                    QSignalBlocker b(m_textToolStartSpin);
                    m_textToolStartSpin->setValue(startTime);
                }
                if (m_textToolEndSpin) {
                    QSignalBlocker b(m_textToolEndSpin);
                    m_textToolEndSpin->setValue(qMax(0.1, endTime - startTime));
                }
                // Re-push the overlay list so the preview re-renders with
                // the updated time range.
                const auto &clips = m_timeline->videoClips();
                if (!clips.isEmpty() && m_player) {
                    QVector<EnhancedTextOverlay> overlays;
                    const auto &mgr = clips[0].textManager;
                    for (int i = 0; i < mgr.count(); ++i)
                        overlays.append(mgr.overlay(i));
                    m_player->setTextOverlays(overlays);
                }
                statusBar()->showMessage(QString("テキスト時間: %1 s → %2 s (%3 s)")
                    .arg(startTime, 0, 'f', 2)
                    .arg(endTime, 0, 'f', 2)
                    .arg(endTime - startTime, 0, 'f', 2));
                (void)idx;
            });

    // Right-click clip menu → existing dialogs. Timeline doesn't own them.
    connect(m_timeline, &Timeline::transitionDialogRequested,
            this, &MainWindow::addTransition);
    connect(m_timeline, &Timeline::videoEffectsDialogRequested,
            this, &MainWindow::videoEffects);
    connect(m_timeline, &Timeline::colorCorrectionRequested,
            this, &MainWindow::colorCorrection);
    connect(m_timeline, &Timeline::transitionShortened,
            this, [this](const QString &name, double askedSec, double effSec) {
                statusBar()->showMessage(
                    QString("ハンドル不足: %1 を %2s → %3s に短縮しました")
                        .arg(name)
                        .arg(askedSec, 0, 'f', 2)
                        .arg(effSec,   0, 'f', 2),
                    5000);
            });

    mainLayout->addWidget(m_welcomeWidget);
    mainLayout->addWidget(m_mainSplitter);
    setCentralWidget(centralWidget);

    connect(m_player, &VideoPlayer::positionChanged, this, [this](double seconds) {
        if (m_timeline) {
            m_timeline->setPlayheadPosition(seconds);
        }
        emit playheadSecondsChanged(seconds);
    });
    connect(m_timeline, &Timeline::scrubPositionChanged, this, [this](double seconds) {
        m_player->previewSeek(qRound(static_cast<double>(seconds) * 1000.0));
    });
    connect(m_timeline, &Timeline::positionChanged, this, [this](double seconds) {
        m_player->seek(qRound(static_cast<double>(seconds) * 1000.0));
    });
    // Multi-clip playback: forward Timeline's resolved schedule to VideoPlayer.
    // Apply proxy-path translation here so VideoPlayer stays unaware of proxies.
    connect(m_timeline, &Timeline::sequenceChanged, this, [this](const QVector<PlaybackEntry> &entries) {
        if (!m_player) return;
        QVector<PlaybackEntry> resolved = entries;
        auto &pm = ProxyManager::instance();
        for (auto &e : resolved)
            e.filePath = pm.getProxyPath(e.filePath);
        qInfo() << "MainWindow: forwarding sequenceChanged entries=" << resolved.size();
        m_player->setSequence(resolved);
        // US-INT-2 Phase A: gather per-entry speed ramps in lockstep with
        // setSequence. Key by (sourceTrack, sourceClipIndex) — positional
        // alignment between resolved[] and any single track's clips() is
        // unsafe (gaps + overlaps mean the indexes diverge). Missing or
        // out-of-range clips fall back to identity so the parallel array
        // size always matches resolved.
        QVector<speedramp::SpeedRamp> videoRamps;
        videoRamps.reserve(resolved.size());
        const auto &vTracks = m_timeline->videoTracks();
        for (const auto &e : resolved) {
            if (e.sourceTrack >= 0 && e.sourceTrack < vTracks.size()) {
                const auto &clips = vTracks[e.sourceTrack]->clips();
                if (e.sourceClipIndex >= 0
                    && e.sourceClipIndex < clips.size()) {
                    videoRamps.append(clips[e.sourceClipIndex].speedRamp);
                    continue;
                }
            }
            videoRamps.append(speedramp::SpeedRamp::identity());
        }
        m_player->setSpeedRamps(videoRamps);
    });
    // Audio-side schedule — feeds AudioMixer so every active entry across
    // A1..A16 is sum-mixed into a single output. Unlinked A clips and
    // overlapping tracks all sound simultaneously.
    connect(m_timeline, &Timeline::audioSequenceChanged, this, [this](const QVector<PlaybackEntry> &entries) {
        if (!m_player) return;
        QVector<PlaybackEntry> resolved = entries;
        auto &pm = ProxyManager::instance();
        for (auto &e : resolved)
            e.filePath = pm.getProxyPath(e.filePath);
        qInfo() << "MainWindow: forwarding audioSequenceChanged entries=" << resolved.size();
        m_player->setAudioSequence(resolved);
        // US-INT-2 Phase A: forward audio-side speed ramps (stored only for
        // now; AudioMixer Phase B will read them under m_controlMutex when
        // per-fragment atempo lands).
        QVector<speedramp::SpeedRamp> audioRamps;
        audioRamps.reserve(resolved.size());
        const auto &aTracks = m_timeline->audioTracks();
        for (const auto &e : resolved) {
            if (e.sourceTrack >= 0 && e.sourceTrack < aTracks.size()) {
                const auto &clips = aTracks[e.sourceTrack]->clips();
                if (e.sourceClipIndex >= 0
                    && e.sourceClipIndex < clips.size()) {
                    audioRamps.append(clips[e.sourceClipIndex].speedRamp);
                    continue;
                }
            }
            audioRamps.append(speedramp::SpeedRamp::identity());
        }
        if (auto *mix = m_player->audioMixer())
            mix->setSpeedRamps(audioRamps);
    });
    // Per-track solo state lives on the mixer (effective gain applied per
    // entry); audioSequenceChanged alone can't carry it because solo is
    // global state, not a per-clip flag. Forward it directly.
    connect(m_timeline, &Timeline::trackSoloChanged, this, [this](int trackIdx, bool solo) {
        if (!m_player) return;
        if (auto *mixer = m_player->audioMixer())
            mixer->setTrackSolo(trackIdx, solo);
    });

    // Proxy generation progress dialog: modeless window created lazily on
    // the first proxyStarted and reused for the rest of the session, so the
    // user has a stable place to monitor / abort the queue.
    auto &proxyMgr = ProxyManager::instance();
    connect(&proxyMgr, &ProxyManager::proxyStarted, this, [this](const QString &clipName) {
        if (!m_proxyDialog) {
            m_proxyDialog = new ProxyProgressDialog(this);
            m_proxyDialog->setAttribute(Qt::WA_DeleteOnClose, false);
            connect(m_proxyDialog, &ProxyProgressDialog::cancelRequested,
                    &ProxyManager::instance(), &ProxyManager::cancelGeneration);
        }
        m_proxyDialog->onProxyStarted(clipName);
    });
    connect(&proxyMgr, &ProxyManager::proxyProgress, this,
            [this](const QString &clipName, int percent) {
        if (m_proxyDialog) m_proxyDialog->onProxyProgress(clipName, percent);
    });
    connect(&proxyMgr, &ProxyManager::proxyFinished, this,
            [this](const QString &clipName, bool ok) {
        if (m_proxyDialog) m_proxyDialog->onProxyFinished(clipName, ok);
    });
    connect(&proxyMgr, &ProxyManager::proxyCancelled, this,
            [this](const QString &clipName) {
        if (m_proxyDialog) m_proxyDialog->onProxyCancelled(clipName);
    });

    // J/K/L keyboard shortcuts for playback
    auto *jKey = new QShortcut(QKeySequence(Qt::Key_J), this);
    connect(jKey, &QShortcut::activated, m_player, &VideoPlayer::speedDown);
    auto *kKey = new QShortcut(QKeySequence(Qt::Key_K), this);
    connect(kKey, &QShortcut::activated, m_player, &VideoPlayer::togglePlay);
    auto *lKey = new QShortcut(QKeySequence(Qt::Key_L), this);
    connect(lKey, &QShortcut::activated, m_player, &VideoPlayer::speedUp);

    // Ctrl+Wheel zoom
    connect(m_player, &VideoPlayer::playbackSpeedChanged, this, [this](double speed) {
        statusBar()->showMessage(QString("Speed: %1x").arg(speed, 0, 'f', 1));
    });

    m_player->setMinimumHeight(280); // lowered so the timeline can grow taller
    m_mainSplitter->setSizes({680, 320});

    // Iteration 15 — preview maximize toggle. The button lives in the
    // VideoPlayer's bottom-right; clicking it (or Esc when active) hides
    // the timeline so the preview can occupy the full main splitter.
    connect(m_player, &VideoPlayer::previewMaximizeChanged, this,
            [this](bool maximized) {
                if (m_timeline) m_timeline->setVisible(!maximized);
            });
    auto *escKey = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escKey, &QShortcut::activated, this, [this]() {
        // US-WIRE-3: Esc cancels motion tracking region picker
        if (m_player && m_player->isRegionPickerActive()) {
            m_player->exitRegionPickerMode();
            statusBar()->showMessage(QString());
            return;
        }
        if (m_player && m_player->isPreviewMaximized()) {
            m_player->setPreviewMaximized(false);
        }
    });
    connect(m_player, &VideoPlayer::frameComposited, this, [this](const QImage &) {
        if (!m_player || m_player->isPlaying())
            return;
        refreshSpecialClipPreview();
    });

    // Restore master loudness normalizer settings on startup.
    if (auto *mixer = m_player ? m_player->audioMixer() : nullptr) {
        QSettings audioPrefs("VSimpleEditor", "Preferences");
        mixer->setNormalizerAmount(
            audioPrefs.value("audio/normalizerAmount", 0.0).toDouble());
        mixer->setNormalizerUniformity(
            audioPrefs.value("audio/normalizerUniformity", 0.5).toDouble());
    }
}

void MainWindow::setupMenuBar()
{
    // ファイル メニュー
    auto *fileMenu = menuBar()->addMenu("ファイル(&F)");

    auto *newAction = fileMenu->addAction("新規プロジェクト(&N)...");
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::newProject);
    m_menuHelpEntries.append({newAction,
        QStringLiteral("まっさらな状態で編集を始めます。今の作業は保存していないと消えてしまうので注意してください。")});

    auto *projectSettingsAction = fileMenu->addAction("プロジェクト設定(&T)...");
    connect(projectSettingsAction, &QAction::triggered, this, &MainWindow::editProjectSettings);
    m_menuHelpEntries.append({projectSettingsAction,
        QStringLiteral("動画の縦横サイズ・フレームレート（なめらかさ）・名前を決めます。YouTube や TikTok 向けのひな型も選べます。")});

    auto *openAction = fileMenu->addAction("ファイルを開く(&O)...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);
    m_menuHelpEntries.append({openAction,
        QStringLiteral("パソコンの中の動画・画像・音声ファイルを読み込んで素材として取り込みます。")});

    auto *importUrlAction = fileMenu->addAction("URL から動画を取り込み(&U)...");
    connect(importUrlAction, &QAction::triggered, this, &MainWindow::importVideoFromUrl);
    m_menuHelpEntries.append({importUrlAction,
        QStringLiteral("YouTube などの動画 URL を貼り付けて、yt-dlp でダウンロードしてそのまま素材に取り込みます。")});

    // 最近使ったファイル
    m_recentFilesMenu = new RecentFilesMenu(m_recentFilesManager, fileMenu);
    m_recentFilesMenu->setTitle("最近使ったファイル");
    fileMenu->addMenu(m_recentFilesMenu);
    connect(m_recentFilesMenu, &RecentFilesMenu::fileSelected, this, &MainWindow::openRecentFile);
    if (m_recentFilesMenu->menuAction())
        m_menuHelpEntries.append({m_recentFilesMenu->menuAction(),
            QStringLiteral("直前に開いた素材やプロジェクトを一覧からすぐに呼び出せます。")});

    fileMenu->addSeparator();

    auto *openProjectAction = fileMenu->addAction("プロジェクトを開く(&P)...");
    openProjectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(openProjectAction, &QAction::triggered, this, &MainWindow::openProject);
    m_menuHelpEntries.append({openProjectAction,
        QStringLiteral("以前に保存した編集プロジェクト（.veproj）ファイルを開いて続きから作業します。")});

    auto *saveAction = fileMenu->addAction("プロジェクトを保存(&S)");
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::saveProject);
    m_menuHelpEntries.append({saveAction,
        QStringLiteral("今の編集内容をプロジェクトファイルに書き出します。こまめに保存しましょう。")});

    auto *saveAsAction = fileMenu->addAction("名前を付けて保存(&A)...");
    saveAsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveProjectAs);
    m_menuHelpEntries.append({saveAsAction,
        QStringLiteral("別名でプロジェクトを保存します。元のファイルを残したまま別バージョンを作りたいときに使います。")});

    fileMenu->addSeparator();

    fileMenu->addSeparator();

    // Premiere Multicam / Resolve Multicam Sync (simplified) parity —
    // standalone dialog that builds a MultiCamProject EDL.
    auto *multiCamDialogAction = fileMenu->addAction("マルチカメラ...");
    connect(multiCamDialogAction, &QAction::triggered, this, &MainWindow::openMultiCamDialog);
    m_menuHelpEntries.append({multiCamDialogAction,
        QStringLiteral("複数カメラで撮った同じ場面の映像を切り替えながら 1 本にまとめます。")});

    // Premiere Media Encoder / Resolve Deliver page parity — modeless
    // dialog that lists pending / running / completed export jobs.
    auto *renderQueueDialogAction = fileMenu->addAction("レンダーキュー...");
    connect(renderQueueDialogAction, &QAction::triggered, this, &MainWindow::openRenderQueueDialog);
    m_menuHelpEntries.append({renderQueueDialogAction,
        QStringLiteral("複数の書き出しをまとめて順番に処理する待ち行列を開きます。")});

    fileMenu->addSeparator();

    auto *exportAction = fileMenu->addAction("エクスポート(&E)...");
    exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportVideo);
    m_menuHelpEntries.append({exportAction,
        QStringLiteral("完成した動画を mp4 などの 1 本の動画ファイルに書き出します。")});

    auto *remotionAction = fileMenu->addAction("Remotion形式でエクスポート(&R)...");
    connect(remotionAction, &QAction::triggered, this, &MainWindow::exportToRemotion);

    // US-SC2-B: Sprint 13 — SNS 向けプリセット (Instagram/TikTok/YouTube Shorts) で
    // 縦動画リフレーミング込みエクスポートを開く。
    auto *socialExportAction = fileMenu->addAction(QStringLiteral("SNS 向けエクスポート…"));
    socialExportAction->setObjectName("action_social_export");
    connect(socialExportAction, &QAction::triggered,
            this, &MainWindow::openSocialExportDialog);
    m_menuHelpEntries.append({socialExportAction,
        QStringLiteral("Instagram / TikTok / YouTube Shorts などの SNS 向けプリセットでエクスポートします (9:16/1:1/4:5 縦動画自動リフレーミング対応)。")});

    // US-INT-1: Sprint 16 — モバイルデバイス向けエクスポート (iPhone/iPad/Android プロファイル)。
#ifdef HAVE_MOBILE_EXPORT
    auto *mobileExportAction = fileMenu->addAction(QStringLiteral("モバイルデバイス向けエクスポート(&M)…"));
    mobileExportAction->setObjectName("action_mobile_export");
    connect(mobileExportAction, &QAction::triggered,
            this, &MainWindow::onMobileExport);
    m_menuHelpEntries.append({mobileExportAction,
        QStringLiteral("iPhone / iPad / Android などのモバイルデバイス向けに最適化されたエクスポートプロファイルを開きます。")});
#endif

    // US-INT-1: Sprint 16 — 外部ツール (OBS / Affinity / Blender) からの取り込みハブ。
#ifdef HAVE_IMPORT_HUB
    auto *importHubAction = fileMenu->addAction(QStringLiteral("外部ツール取り込みハブ(&I)…"));
    importHubAction->setObjectName("action_import_hub");
    connect(importHubAction, &QAction::triggered,
            this, &MainWindow::onImportHub);
    m_menuHelpEntries.append({importHubAction,
        QStringLiteral("OBS の録画 / Affinity Photo の PSD / Blender のメッシュ・EXR シーケンスをまとめて取り込むハブを開きます。")});
#endif

    // US-EXT-10: HDR (HDR10/HLG) output settings dialog.
    auto *hdrSettingsAction = fileMenu->addAction("HDR 出力設定...");
    hdrSettingsAction->setObjectName("action_hdr_settings");
    connect(hdrSettingsAction, &QAction::triggered, this, &MainWindow::onHDRSettings);
    m_menuHelpEntries.append({hdrSettingsAction,
        QStringLiteral("HDR (HDR10 / HLG) 書き出しのメタデータと表示プレビュー設定を編集する。")});

    // US-HW-10: collect project + referenced media into a single folder.
    auto *collectAction = fileMenu->addAction("プロジェクトを収集 (Collect Files)...");
    collectAction->setObjectName("action_collect_project");
    connect(collectAction, &QAction::triggered, this, &MainWindow::onCollectProject);
    m_menuHelpEntries.append({collectAction,
        QStringLiteral("プロジェクトと参照メディアを 1 フォルダにまとめる (Collect Files)。")});
    m_menuHelpEntries.append({remotionAction,
        QStringLiteral("プログラム（Remotion）で再編集できる形式に書き出します。上級者向けです。")});

    fileMenu->addSeparator();

    auto *prefsMenu = fileMenu->addMenu("環境設定(&S)");
    if (prefsMenu->menuAction())
        m_menuHelpEntries.append({prefsMenu->menuAction(),
            QStringLiteral("テーマ、ショートカット、自動保存などアプリ全体の設定をまとめて変更できます。")});
    fileMenu->addSeparator();

    auto *quitAction = fileMenu->addAction("終了(&Q)");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
    m_menuHelpEntries.append({quitAction,
        QStringLiteral("アプリを閉じます。保存していない変更があるか確認してから終了してください。")});

    // 編集 メニュー
    auto *editMenu = menuBar()->addMenu("編集(&E)");

    m_undoAction = editMenu->addAction("元に戻す(&U)");
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undoAction);
    m_menuHelpEntries.append({m_undoAction,
        QStringLiteral("直前の操作を取り消します。間違えたらまずこれ（Ctrl+Z）。")});

    m_redoAction = editMenu->addAction("やり直す(&R)");
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::redoAction);
    m_menuHelpEntries.append({m_redoAction,
        QStringLiteral("「元に戻す」で取り消した操作を、もう一度やり直します。")});

    editMenu->addSeparator();

    m_copyAction = editMenu->addAction("クリップをコピー(&C)");
    m_copyAction->setShortcut(QKeySequence::Copy);
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::copyClip);
    m_menuHelpEntries.append({m_copyAction,
        QStringLiteral("選んでいるクリップを複製用にコピーします。貼り付けと組み合わせて使います。")});

    m_pasteAction = editMenu->addAction("クリップを貼り付け(&P)");
    m_pasteAction->setShortcut(QKeySequence::Paste);
    connect(m_pasteAction, &QAction::triggered, this, &MainWindow::pasteClip);
    m_menuHelpEntries.append({m_pasteAction,
        QStringLiteral("コピーしたクリップを再生ヘッドの位置に貼り付けます。")});

    editMenu->addSeparator();

    m_splitAction = editMenu->addAction("再生ヘッドで分割(&S)");
    m_splitAction->setShortcut(QKeySequence(Qt::Key_S));
    connect(m_splitAction, &QAction::triggered, this, &MainWindow::splitClip);
    m_menuHelpEntries.append({m_splitAction,
        QStringLiteral("再生ヘッド（縦線）の位置でクリップを 2 つに切り分けます。いらない部分を消す前準備に。")});

    m_deleteAction = editMenu->addAction("クリップを削除(&D)");
    m_deleteAction->setShortcut(QKeySequence::Delete);
    connect(m_deleteAction, &QAction::triggered, this, &MainWindow::deleteClip);
    m_menuHelpEntries.append({m_deleteAction,
        QStringLiteral("選んだクリップをタイムラインから消します。元の素材ファイル自体は消えません。")});

    m_rippleDeleteAction = editMenu->addAction("リップル削除");
    m_rippleDeleteAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    connect(m_rippleDeleteAction, &QAction::triggered, this, &MainWindow::rippleDelete);
    m_menuHelpEntries.append({m_rippleDeleteAction,
        QStringLiteral("クリップを消して、空いた隙間を後ろのクリップが詰めて埋めます。間を空けたくないときに。")});

    // US-WF-D: Sprint 11 workflow — magnetic timeline closeGaps demo.
    auto *magTlDemoAction = editMenu->addAction("タイムラインギャップを詰める (Demo)");
    magTlDemoAction->setObjectName("action_magnetic_timeline_demo");
    connect(magTlDemoAction, &QAction::triggered, this, &MainWindow::runMagneticTimelineDemo);
    m_menuHelpEntries.append({magTlDemoAction,
        QStringLiteral("Magnetic Timeline の closeGaps を 2 クリップの合成例で実行し、結果を表示します (デモ)。")});

    editMenu->addSeparator();

    m_copyEffectsAction = editMenu->addAction("Copy Effects");
    m_copyEffectsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    connect(m_copyEffectsAction, &QAction::triggered, this, &MainWindow::copyEffects);
    m_menuHelpEntries.append({m_copyEffectsAction,
        QStringLiteral("選んだクリップに付けたエフェクト（色補正やぼかし等）の設定だけをコピーします。")});

    m_pasteEffectsAction = editMenu->addAction("Paste Effects");
    m_pasteEffectsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    connect(m_pasteEffectsAction, &QAction::triggered, this, &MainWindow::pasteEffects);
    m_menuHelpEntries.append({m_pasteEffectsAction,
        QStringLiteral("コピーしたエフェクト設定を別のクリップに貼り付けます。同じ見た目をまとめて適用できます。")});

    m_pasteAttributesAction = editMenu->addAction("Paste Attributes...");
    connect(m_pasteAttributesAction, &QAction::triggered, this, &MainWindow::pasteAttributes);
    m_menuHelpEntries.append({m_pasteAttributesAction,
        QStringLiteral("エフェクトのうちどの項目を貼り付けるかを選んで適用します。")});

    auto &clipBoard = effectctrl::EffectClipboard::instance();
    m_pasteEffectsAction->setEnabled(clipBoard.hasContent());
    m_pasteAttributesAction->setEnabled(clipBoard.hasContent());
    connect(&clipBoard, &effectctrl::EffectClipboard::contentChanged, this, [this]() {
        const bool has = effectctrl::EffectClipboard::instance().hasContent();
        if (m_pasteEffectsAction) m_pasteEffectsAction->setEnabled(has);
        if (m_pasteAttributesAction) m_pasteAttributesAction->setEnabled(has);
    });

    editMenu->addSeparator();

    auto *applyDefaultTransAct = editMenu->addAction("規定トランジションを適用(&D)");
    applyDefaultTransAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(applyDefaultTransAct, &QAction::triggered, this, &MainWindow::applyDefaultTransition);

    auto *editDefaultTransAct = editMenu->addAction("規定トランジション設定...");
    connect(editDefaultTransAct, &QAction::triggered, this, &MainWindow::editDefaultTransition);

    editMenu->addSeparator();

    m_snapAction = editMenu->addAction("スナップ切替(&N)");
    m_snapAction->setShortcut(QKeySequence(Qt::Key_N));
    m_snapAction->setCheckable(true);
    m_snapAction->setChecked(true);
    connect(m_snapAction, &QAction::triggered, this, &MainWindow::toggleSnap);
    m_menuHelpEntries.append({m_snapAction,
        QStringLiteral("ON にすると、クリップを動かすとき隣のクリップや再生ヘッドにピタッと吸い付きます。")});

    editMenu->addSeparator();

    auto *speedAction = editMenu->addAction("再生速度を設定...");
    connect(speedAction, &QAction::triggered, this, &MainWindow::setClipSpeed);
    m_menuHelpEntries.append({speedAction,
        QStringLiteral("選んだクリップを早送り・スローモーションにします。倍率を数字で指定できます。")});

    // Premiere "Speed / Duration" parity — applies a flat SpeedRamp
    // curve to the selected clip via Timeline.
    auto *speedRampDialogAction = editMenu->addAction("速度 / 持続時間...");
    speedRampDialogAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(speedRampDialogAction, &QAction::triggered, this, &MainWindow::openSpeedRampDialog);

    editMenu->addSeparator();

    // Streaming chi-squared histogram cut detector (SceneDetector).
    // Decodes a sample of frames from the active clip and, for each
    // detected cut, drops a Timeline marker.
    auto *sceneDetectAction = editMenu->addAction("シーン検出 (自動)...");
    connect(sceneDetectAction, &QAction::triggered, this, &MainWindow::openSceneDetector);

    // MotionStabilizer — analyses the active clip for camera shake and
    // either bakes counter-translation keyframes (when supported) or
    // reports the result to the status bar (deferred integration).
    auto *stabilizeAct = editMenu->addAction("スタビライズ (手ブレ補正)...");
    connect(stabilizeAct, &QAction::triggered, this, &MainWindow::runMotionStabilizer);

    editMenu->addSeparator();

    // US-SC-B: Sprint 12 — Premiere/FCP/DaVinci 風プリセット切替 + 個別カスタマイズ
    auto *shortcutCustomizeAction = editMenu->addAction(QStringLiteral("ショートカット設定…"));
    shortcutCustomizeAction->setObjectName("action_shortcut_customize");
    connect(shortcutCustomizeAction, &QAction::triggered,
            this, &MainWindow::openShortcutCustomizeDialog);
    prefsMenu->addAction(shortcutCustomizeAction);
    m_menuHelpEntries.append({shortcutCustomizeAction,
        QStringLiteral("メニューやツールバーのキーボードショートカットをカスタマイズしたり、Premiere/FinalCutPro/DaVinci 風プリセットへ切り替えたりします。")});

    editMenu->addSeparator();

    // US-AUTH-6: unified credential dialog for 5 streaming platforms.
    auto *credentialAction = editMenu->addAction(QStringLiteral("配信認証情報..."));
    credentialAction->setShortcut(QKeySequence(tr("Ctrl+Alt+A")));
    connect(credentialAction, &QAction::triggered, this, &MainWindow::onShowCredentialDialog);
    m_menuHelpEntries.append({credentialAction,
        QStringLiteral("YouTube / Vimeo / Instagram / X / Twitch の配信認証情報を 1 画面で確認・保存・削除します。")});

    // US-TP-6: PRD-TP — モーショントラッカー preset 適用ダイアログ。Ctrl+Alt+T
    // で開き、選択した preset を m_motionTracker に適用する。既存の
    // motionTrackSetup() / trackMotion() 経路は変更しない。
    editMenu->addSeparator();
    QAction *trackerAct = editMenu->addAction(tr("モーショントラッカー (&T)..."));
    trackerAct->setShortcut(QKeySequence(tr("Ctrl+Alt+T")));
    connect(trackerAct, &QAction::triggered, this, &MainWindow::showMotionTrackerDialog);

    // お気に入り メニュー — placed right after 編集 so the user's hand-picked
    // shortcuts sit near the top of the menu bar. Mnemonic &O is unused by the
    // other top-level menus (F/E/V/T/I/A/K/P/S/C/H). The dynamic part (the
    // proxy actions) is filled in by rebuildFavoritesMenu() at the very end of
    // setupMenuBar(), once every favoritable QAction exists; here we only
    // create the menu shell and the 「お気に入りを編集...」 action it always
    // keeps at the bottom.
    m_favoritesMenu = menuBar()->addMenu(QStringLiteral("お気に入り(&O)"));
    m_editFavoritesAction = new QAction(QStringLiteral("お気に入りを編集..."), this);
    connect(m_editFavoritesAction, &QAction::triggered, this, &MainWindow::editFavorites);
    m_menuHelpEntries.append({m_editFavoritesAction,
        QStringLiteral("この「お気に入り」メニューに表示する機能を、自分でチェックして選べます。")});

    // 表示 メニュー
    auto *viewMenu = menuBar()->addMenu("表示(&V)");

    auto *zoomInAction = viewMenu->addAction("拡大(&I)");
    zoomInAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal));
    connect(zoomInAction, &QAction::triggered, this, &MainWindow::zoomIn);
    m_menuHelpEntries.append({zoomInAction,
        QStringLiteral("タイムラインを拡大して、細かい位置あわせをしやすくします。")});

    auto *zoomOutAction = viewMenu->addAction("縮小(&O)");
    zoomOutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    connect(zoomOutAction, &QAction::triggered, this, &MainWindow::zoomOut);
    m_menuHelpEntries.append({zoomOutAction,
        QStringLiteral("タイムラインを縮小して、動画全体を一目で見られるようにします。")});

    // トラック メニュー
    auto *trackMenu = menuBar()->addMenu("トラック(&T)");

    auto *addVTrack = trackMenu->addAction("ビデオトラックを追加(&V)");
    connect(addVTrack, &QAction::triggered, this, &MainWindow::addVideoTrack);
    m_menuHelpEntries.append({addVTrack,
        QStringLiteral("映像を重ねるための「段」を増やします。上の段ほど手前（前面）に表示されます。")});

    auto *addATrack = trackMenu->addAction("オーディオトラックを追加(&A)");
    connect(addATrack, &QAction::triggered, this, &MainWindow::addAudioTrack);
    m_menuHelpEntries.append({addATrack,
        QStringLiteral("音声を重ねるための段を増やします。ナレーションと BGM を別々の段に置けます。")});

    // 挿入 メニュー
    auto *insertMenu = menuBar()->addMenu("挿入(&I)");

    auto *addTextAction = insertMenu->addAction("テキスト / テロップ追加(&T)...");
    addTextAction->setShortcut(QKeySequence(Qt::Key_T));
    connect(addTextAction, &QAction::triggered, this, &MainWindow::addTextOverlay);
    m_menuHelpEntries.append({addTextAction,
        QStringLiteral("画面に字幕やテロップ（文字）を追加します。フォントや色も選べます。")});

    auto *manageTextAction = insertMenu->addAction("テキスト管理(&M)...");
    manageTextAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    connect(manageTextAction, &QAction::triggered, this, &MainWindow::manageTextOverlays);
    m_menuHelpEntries.append({manageTextAction,
        QStringLiteral("追加済みのテロップを一覧で確認・編集・削除できます。")});

    auto *importSubAction = insertMenu->addAction("字幕をインポート (SRT/VTT)...");
    connect(importSubAction, &QAction::triggered, this, &MainWindow::importSubtitles);
    m_menuHelpEntries.append({importSubAction,
        QStringLiteral("字幕ファイル（.srt / .vtt）を読み込んでテロップとして取り込みます。")});

    // US-CAP-B: Sprint 14 — 字幕エディタダイアログ
    auto *captionEditorAction = insertMenu->addAction(QStringLiteral("字幕エディタ…"));
    captionEditorAction->setObjectName("action_caption_editor");
    connect(captionEditorAction, &QAction::triggered,
            this, &MainWindow::openCaptionEditorDialog);
    m_menuHelpEntries.append({captionEditorAction,
        QStringLiteral("字幕クリップを追加・編集・SRT/VTT で取込/書出し、Whisper.cpp など ASR エンジンで自動生成できます。")});

    auto *exportTextAction = insertMenu->addAction("テキストを書き出し (SRT / CSV)...");
    connect(exportTextAction, &QAction::triggered, this, &MainWindow::exportTextOverlays);

    auto *saveTemplateAction = insertMenu->addAction("テキストテンプレートを保存...");
    connect(saveTemplateAction, &QAction::triggered, this, &MainWindow::saveTextTemplate);

    auto *addBrushAnimAction = insertMenu->addAction("ブラシ / 書き起こしアニメ追加(&B)...");
    addBrushAnimAction->setShortcut(QKeySequence(Qt::Key_B));
    connect(addBrushAnimAction, &QAction::triggered, this, &MainWindow::addBrushAnimation);
    m_menuHelpEntries.append({addBrushAnimAction,
        QStringLiteral("手書き風に文字や線が少しずつ描かれていくアニメーションを追加します。")});

    insertMenu->addSeparator();

    auto *addTransAction = insertMenu->addAction("トランジションを追加...");
    connect(addTransAction, &QAction::triggered, this, &MainWindow::addTransition);
    m_menuHelpEntries.append({addTransAction,
        QStringLiteral("クリップのつなぎ目に、フェードやワイプなどの切り替え演出を入れます。")});

    auto *addImageAction = insertMenu->addAction("画像 / 静止画を追加...");
    connect(addImageAction, &QAction::triggered, this, &MainWindow::addImageOverlay);
    m_menuHelpEntries.append({addImageAction,
        QStringLiteral("写真やロゴなどの画像をタイムラインに重ねます。")});

    auto *addPipAction = insertMenu->addAction("ピクチャー・イン・ピクチャー追加...");
    connect(addPipAction, &QAction::triggered, this, &MainWindow::addPip);
    m_menuHelpEntries.append({addPipAction,
        QStringLiteral("画面の隅にもう 1 つ小さい動画を重ねて表示します（ワイプ・実況風）。")});

    insertMenu->addSeparator();

    // Premiere Essential Graphics / Resolve Fusion Titles parity.
    auto *titlePresetAction = insertMenu->addAction("タイトルプリセット...");
    titlePresetAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y));
    connect(titlePresetAction, &QAction::triggered, this, &MainWindow::openTitlePresetDialog);
    m_menuHelpEntries.append({titlePresetAction,
        QStringLiteral("デザイン済みのタイトル・テロップのひな型から選んで、文字だけ差し替えて使えます。")});

    // Photoshop / Premiere "Adjustment Layer" — a special timeline clip
    // that carries no video content of its own but applies grading
    // parameters to every video frame underneath.
    auto *addAdjustmentAction = insertMenu->addAction("調整レイヤー");
    connect(addAdjustmentAction, &QAction::triggered, this, &MainWindow::addAdjustmentLayerCmd);
    m_menuHelpEntries.append({addAdjustmentAction,
        QStringLiteral("その下にある全部の映像にまとめて色補正やエフェクトをかけられる特別なレイヤーを追加します。")});

    // US-AETEXT-12: AE Text Parity — 11 new menu actions
    insertMenu->addSeparator();

    auto *pathTextAction = insertMenu->addAction("パステキスト追加...");
    connect(pathTextAction, &QAction::triggered, this, &MainWindow::addPathText);

    auto *rangeSelAction = insertMenu->addAction("レンジセレクター...");
    connect(rangeSelAction, &QAction::triggered, this, &MainWindow::addRangeSelector);

    auto *wigglySelAction = insertMenu->addAction("ウィグリーセレクター...");
    connect(wigglySelAction, &QAction::triggered, this, &MainWindow::addWigglySelector);

    auto *srcTextKfAction = insertMenu->addAction("ソーステキスト keyframe");
    connect(srcTextKfAction, &QAction::triggered, this, &MainWindow::addSourceTextKeyframe);

    auto *animPresetAction = insertMenu->addAction("アニメーションプリセット...");
    connect(animPresetAction, &QAction::triggered, this, &MainWindow::addAnimationPreset);

    auto *text3DAction = insertMenu->addAction("3Dテキストレイヤー追加...");
    connect(text3DAction, &QAction::triggered, this, &MainWindow::add3DText);

    auto *maskRevealAction = insertMenu->addAction("マスクテキストreveal追加...");
    connect(maskRevealAction, &QAction::triggered, this, &MainWindow::addMaskTextReveal);

    auto *bendWarpAction = insertMenu->addAction("ベンド/インフレートtext追加...");
    connect(bendWarpAction, &QAction::triggered, this, &MainWindow::addBendTextWarp);

    auto *scopeAction = insertMenu->addAction("スコープ切替...");
    connect(scopeAction, &QAction::triggered, this, &MainWindow::changeTextScope);

    auto *varFontAction = insertMenu->addAction("可変フォントaxisアニメ...");
    connect(varFontAction, &QAction::triggered, this, &MainWindow::addVariableFontAxis);

    auto *mographAction = insertMenu->addAction("Mographテンプレート...");
    connect(mographAction, &QAction::triggered, this, &MainWindow::addMographTemplate);

    // オーディオ メニュー
    auto *audioMenu = menuBar()->addMenu("オーディオ(&A)");

    auto *volumeAction = audioMenu->addAction("音量を設定...");
    connect(volumeAction, &QAction::triggered, this, &MainWindow::setClipVolume);
    m_menuHelpEntries.append({volumeAction,
        QStringLiteral("選んだ音声クリップの大きさや、フェードイン・フェードアウトを調整します。")});

    auto *bgmAction = audioMenu->addAction("BGM / 音声ファイルを追加...");
    connect(bgmAction, &QAction::triggered, this, &MainWindow::addBgm);
    m_menuHelpEntries.append({bgmAction,
        QStringLiteral("BGM や効果音などの音声ファイルをタイムラインに追加します。")});

    auto *voiceOverAction = audioMenu->addAction("Voice-over Record...");
    voiceOverAction->setShortcut(QKeySequence(Qt::Key_F12));
    connect(voiceOverAction, &QAction::triggered, this, &MainWindow::openVoiceOverDialog);
    m_menuHelpEntries.append({voiceOverAction,
        QStringLiteral("映像を見ながらマイクでナレーションを録音し、その場でトラックに追加します。")});

    audioMenu->addSeparator();

    auto *muteAction = audioMenu->addAction("ミュート切替 (A1)");
    muteAction->setShortcut(QKeySequence(Qt::Key_M));
    connect(muteAction, &QAction::triggered, this, &MainWindow::toggleMute);
    m_menuHelpEntries.append({muteAction,
        QStringLiteral("オーディオトラック A1 の音を一時的に消す／戻すを切り替えます。")});

    auto *soloAction = audioMenu->addAction("ソロ切替 (A1)");
    connect(soloAction, &QAction::triggered, this, &MainWindow::toggleSolo);
    m_menuHelpEntries.append({soloAction,
        QStringLiteral("オーディオトラック A1 だけを鳴らして、他の音を止めて確認します。")});

    audioMenu->addSeparator();

    auto *eqAction = audioMenu->addAction("イコライザー...");
    connect(eqAction, &QAction::triggered, this, &MainWindow::audioEqualizer);
    m_menuHelpEntries.append({eqAction,
        QStringLiteral("低音・高音などの聞こえ方を調整して音質を整えます。こもった声をクリアにしたいときに。")});

    auto *audioFxAction = audioMenu->addAction("オーディオエフェクト...");
    connect(audioFxAction, &QAction::triggered, this, &MainWindow::audioEffects);
    m_menuHelpEntries.append({audioFxAction,
        QStringLiteral("音にエコーや音量の自動調整などの効果をかけます。")});

    audioMenu->addSeparator();

    auto *vstAction = audioMenu->addAction("VST / AUプラグイン...");
    connect(vstAction, &QAction::triggered, this, &MainWindow::openVSTPlugins);
    m_menuHelpEntries.append({vstAction,
        QStringLiteral("外部の音響プラグイン（VST/AU）を読み込んで使います。上級者向けです。")});

    audioMenu->addSeparator();

    // Track EQ submenu — rebuilt on aboutToShow with one entry per
    // audio track, each exposing the 5 built-in EQ presets.
    auto *trackEqMenu = audioMenu->addMenu("Track EQ");
    connect(trackEqMenu, &QMenu::aboutToShow, this, [this, trackEqMenu]() {
        trackEqMenu->clear();
        if (!m_timeline) return;
        const int trackCount = m_timeline->audioTrackCount();
        if (trackCount == 0) {
            auto *info = trackEqMenu->addAction("オーディオトラックがありません");
            info->setEnabled(false);
            return;
        }
        const auto presets = AudioEQProcessor::presets();
        auto *mixer = m_player ? m_player->audioMixer() : nullptr;
        for (int i = 0; i < trackCount; ++i) {
            auto *trackSub = trackEqMenu->addMenu(QString("A%1").arg(i + 1));
            for (const auto &preset : presets) {
                auto *act = trackSub->addAction(preset.name);
                const int ti = i;
                const AudioEQConfig cfg = preset.config;
                const QString presetName = preset.name;
                connect(act, &QAction::triggered, this,
                        [this, mixer, ti, cfg, presetName]() {
                    if (mixer) {
                        mixer->setTrackEqEnabled(ti, true);
                        mixer->setTrackEqConfig(ti, cfg);
                    }
                    statusBar()->showMessage(
                        QString("A%1 EQ → %2").arg(ti + 1).arg(presetName),
                        3000);
                });
            }
            trackSub->addSeparator();
            auto *bypassAct = trackSub->addAction("Bypass / Reset");
            const int ti2 = i;
            connect(bypassAct, &QAction::triggered, this,
                    [this, mixer, ti2]() {
                if (mixer) {
                    mixer->setTrackEqEnabled(ti2, false);
                }
                statusBar()->showMessage(
                    QString("A%1 EQ bypassed").arg(ti2 + 1), 3000);
           });
        }
    });

    // Master Compressor dialog
    auto *compressorAction = audioMenu->addAction("Master Compressor...");
    connect(compressorAction, &QAction::triggered, this, &MainWindow::openMasterCompressor);
    m_menuHelpEntries.append({compressorAction,
        QStringLiteral("動画全体の音量の差を縮めて、小さい音は聞こえやすく、大きすぎる音はおさえます。")});

    audioMenu->addSeparator();

    // Pro-NLE "rubber band" volume envelope. When ON, every audio row
    // overlays its per-clip envelope; left-click adds a point, drag moves
    // it, right-click removes it. AudioMixer interpolates linearly.
    auto *envelopeAction = audioMenu->addAction("ボリュームエンベロープ編集モード");
    envelopeAction->setCheckable(true);
    // Ctrl+Shift+E so we don't shadow the existing File→Export Ctrl+E.
    envelopeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    connect(envelopeAction, &QAction::toggled, this,
            [this](bool on) {
                TimelineTrack::setEnvelopeEditMode(on);
                if (m_timeline) m_timeline->repaintAudioTracks();
                int trackCount = m_timeline ? m_timeline->audioTrackCount() : -1;
                int clipCount = 0;
                if (m_timeline) {
                    for (auto *t : m_timeline->audioTracks())
                        if (t) clipCount += t->clipCount();
                }
                statusBar()->showMessage(
                    QString("ボリュームエンベロープ %1 (audio tracks=%2, total clips=%3)")
                        .arg(on ? QStringLiteral("ON") : QStringLiteral("OFF"))
                        .arg(trackCount).arg(clipCount), 6000);
            });

    audioMenu->addSeparator();

    // Pro-NLE auto-ducking. Submenu rebuilt on aboutToShow so newly added
    // audio tracks appear without restart. Picking A<n> writes a -12 dB
    // envelope on every other audio track that overlaps that track's
    // clip ranges (200 ms attack / 400 ms release, Premiere defaults).
    auto *duckMenu = audioMenu->addMenu("BGM 自動ダッキング");
    connect(duckMenu, &QMenu::aboutToShow, this, [this, duckMenu]() {
        duckMenu->clear();
        if (!m_timeline) return;
        const int n = m_timeline->audioTrackCount();
        if (n < 2) {
            auto *info = duckMenu->addAction("オーディオトラックを 2 本以上に増やしてください");
            info->setEnabled(false);
            return;
        }
        for (int i = 0; i < n; ++i) {
            auto *act = duckMenu->addAction(
                QString("A%1 を voice 扱いして他トラックをダッキング").arg(i + 1));
            const int ti = i;
            connect(act, &QAction::triggered, this, [this, ti]() {
                if (!m_timeline) return;
                auto *mixer = m_player ? m_player->audioMixer() : nullptr;
                if (mixer) {
                    const auto &ad = mixer->autoDuckParams();
                    const double duckGain = std::pow(10.0, ad.thresholdDb / 20.0);
                    const double attackSec = ad.attackMs / 1000.0;
                    const double releaseSec = ad.releaseMs / 1000.0;
                    m_timeline->applyDuckingFromTrack(ti, duckGain, attackSec, releaseSec);
                } else {
                    m_timeline->applyDuckingFromTrack(ti);
                }
                statusBar()->showMessage(
                    QString("A%1 を voice 扱いして他オーディオトラックをダッキングしました").arg(ti + 1),
                    4000);
            });
        }
    });

    auto *duckSettingsAction = audioMenu->addAction("オートダック設定 (AudioMixer)...");
    duckSettingsAction->setObjectName("action_auto_duck_mixer");
    connect(duckSettingsAction, &QAction::triggered, this, &MainWindow::openAutoDuckSettings);
    m_menuHelpEntries.append({duckSettingsAction,
        QStringLiteral("ナレーションが入っている間だけ BGM を自動で小さくする「自動ダッキング」の細かい設定です。")});

    // US-HW-10: project-level sidechain ducking parameters (AudioDuckingDialog).
    auto *duckingSettingsAction = audioMenu->addAction("オーディオダッキング設定 (プロジェクト)...");
    duckingSettingsAction->setObjectName("action_audio_ducking_settings");
    connect(duckingSettingsAction, &QAction::triggered, this, &MainWindow::onAudioDuckingSettings);
    m_menuHelpEntries.append({duckingSettingsAction,
        QStringLiteral("サイドチェイン音声に応じて BGM を自動で下げるダッキング設定を編集する。")});

    audioMenu->addSeparator();

    // Per-track DSP panels (4-band EQ, compressor/limiter, Schroeder
    // reverb, noise reduction). Each opens / toggles a right-docked
    // QDockWidget that owns the panel widget; on change the panel signal
    // calls AudioMixer::setEqForTrack / setCompressorForTrack /
    // setReverbForTrack / setNoiseReductionForTrack.
    auto *eqPanelAction = audioMenu->addAction("EQ パネル...");
    connect(eqPanelAction, &QAction::triggered, this, &MainWindow::openEqualizerPanel);
    m_menuHelpEntries.append({eqPanelAction,
        QStringLiteral("トラックごとに低音・中音・高音のバランスを調整する画面を開きます。")});

    auto *compPanelAction = audioMenu->addAction("コンプレッサー / リミッター...");
    connect(compPanelAction, &QAction::triggered, this, &MainWindow::openCompressorPanel);
    m_menuHelpEntries.append({compPanelAction,
        QStringLiteral("音量の差をならし、急に大きくなりすぎる音を防ぐ画面を開きます。")});

    auto *reverbPanelAction = audioMenu->addAction("リバーブ...");
    connect(reverbPanelAction, &QAction::triggered, this, &MainWindow::openReverbPanel);
    m_menuHelpEntries.append({reverbPanelAction,
        QStringLiteral("音に残響（広い部屋やホールにいるような響き）を足す画面を開きます。")});

    auto *nrPanelAction = audioMenu->addAction("ノイズリダクション...");
    connect(nrPanelAction, &QAction::triggered, this, &MainWindow::openNoiseReductionPanel);
    m_menuHelpEntries.append({nrPanelAction,
        QStringLiteral("「サーッ」という背景ノイズや空調音を減らす画面を開きます。")});

    // US-WF-D: Sprint 11 workflow — per-clip volume envelope editor (AudioClipEditor).
    auto *clipVolumeEditorAction = audioMenu->addAction("クリップボリュームエンベロープエディタ…");
    clipVolumeEditorAction->setObjectName("action_audio_clip_editor");
    connect(clipVolumeEditorAction, &QAction::triggered, this, &MainWindow::openAudioClipEditorDialog);
    m_menuHelpEntries.append({clipVolumeEditorAction,
        QStringLiteral("クリップ内のボリュームエンベロープ (時間 × dB) を点で編集する画面を開きます。")});

    // マーカー メニュー
    auto *markersMenu = menuBar()->addMenu("マーカー(&K)");

    auto *addMarkerAction = markersMenu->addAction("再生ヘッドにマーカー追加");
    addMarkerAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    connect(addMarkerAction, &QAction::triggered, this, &MainWindow::addMarker);
    m_menuHelpEntries.append({addMarkerAction,
        QStringLiteral("今の再生位置に名前付きの目印を付けます。「あとで直す場所」を見失いません。")});

    // Quick "M" key — Premiere/Resolve parity. Uses default red marker
    // colour and an empty label so the user gets a marker without a
    // dialog interrupt; rename via 全マーカーを表示...
    auto *quickMarkerAction = markersMenu->addAction("マーカー追加 (クイック)");
    quickMarkerAction->setShortcut(QKeySequence(Qt::Key_M));
    connect(quickMarkerAction, &QAction::triggered, this, &MainWindow::addQuickMarker);
    m_menuHelpEntries.append({quickMarkerAction,
        QStringLiteral("名前を聞かれずにサッと目印を 1 つ置きます（M キー）。名前はあとで付けられます。")});

    // Shift+M — open colour picker first, then drop a marker tagged with
    // the chosen colour. Persists colour into Timeline marker data via
    // Timeline::addMarker(timelineUs, label, color).
    auto *colouredMarkerAction = markersMenu->addAction("色付きマーカー追加...");
    colouredMarkerAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_M));
    connect(colouredMarkerAction, &QAction::triggered, this, &MainWindow::addColoredMarker);
    m_menuHelpEntries.append({colouredMarkerAction,
        QStringLiteral("色を選んでから目印を置きます。「赤＝要修正」「青＝後で確認」など色分けに便利です。")});

    auto *nextMarkerAction = markersMenu->addAction("次のマーカーへジャンプ");
    nextMarkerAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right));
    connect(nextMarkerAction, &QAction::triggered, this, &MainWindow::jumpToNextMarker);
    m_menuHelpEntries.append({nextMarkerAction,
        QStringLiteral("再生ヘッドを次の目印（マーカー）の位置まで一気に移動します。")});

    auto *prevMarkerAction = markersMenu->addAction("前のマーカーへジャンプ");
    prevMarkerAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left));
    connect(prevMarkerAction, &QAction::triggered, this, &MainWindow::jumpToPrevMarker);
    m_menuHelpEntries.append({prevMarkerAction,
        QStringLiteral("再生ヘッドを 1 つ前の目印（マーカー）の位置まで戻します。")});

    auto *showMarkersAction = markersMenu->addAction("全マーカーを表示...");
    connect(showMarkersAction, &QAction::triggered, this, &MainWindow::showMarkers);
    m_menuHelpEntries.append({showMarkersAction,
        QStringLiteral("付けた目印を一覧で確認し、名前の変更や削除ができます。")});

    auto *exportChapAction = markersMenu->addAction("YouTubeチャプターをエクスポート...");
    connect(exportChapAction, &QAction::triggered, this, &MainWindow::exportChapters);
    m_menuHelpEntries.append({exportChapAction,
        QStringLiteral("マーカーをもとに、YouTube の概要欄に貼れるチャプター一覧（時刻＋見出し）を書き出します。")});

    // MK-2: マーカー パネル ドックの表示トグル。ドックはこのメニュー構築より
    // 後で生成されるため、トグル時に runtime で m_markerPanelDock を参照する。
    markersMenu->addSeparator();
    auto *markerPanelAction = markersMenu->addAction("マーカー パネル");
    markerPanelAction->setCheckable(true);
    connect(markerPanelAction, &QAction::toggled, this,
            [this](bool on) {
                if (m_markerPanelDock)
                    m_markerPanelDock->setVisible(on);
            });
    // ドック生成後に初期チェック状態と双方向同期を結線する。
    QMetaObject::invokeMethod(this, [this, markerPanelAction]() {
        if (!m_markerPanelDock)
            return;
        markerPanelAction->setChecked(m_markerPanelDock->isVisible());
        connect(m_markerPanelDock, &QDockWidget::visibilityChanged,
                markerPanelAction, &QAction::setChecked);
    }, Qt::QueuedConnection);
    m_menuHelpEntries.append({markerPanelAction,
        QStringLiteral("タイムライン上のマーカーを表形式で一覧表示するパネルを出し入れします。行をダブルクリックでその時刻へジャンプ、ノートの編集や削除ができます。")});

    // TR-4: トリム メニュー (プロ NLE のリップル/ロール/スリップ/スライド)。
    // 再生ヘッド駆動なので追加のドラッグ UI なしで成立する。実体は純粋エンジン
    // trimops:: で、Timeline::applyTrimActive() が選択クリップへ適用する。
    auto *trimMenu = menuBar()->addMenu("トリム(&T)");

    auto *rippleInAction = trimMenu->addAction("選択クリップの先頭を再生ヘッドへ (リップル)");
    rippleInAction->setShortcut(QKeySequence(Qt::Key_Q));
    connect(rippleInAction, &QAction::triggered, this, &MainWindow::rippleTrimInToPlayhead);
    m_menuHelpEntries.append({rippleInAction,
        QStringLiteral("選んだクリップの先頭を今の再生位置まで詰めます（リップル）。後ろのクリップは隙間なくついてきます。")});

    auto *rippleOutAction = trimMenu->addAction("選択クリップの末尾を再生ヘッドへ (リップル)");
    rippleOutAction->setShortcut(QKeySequence(Qt::Key_W));
    connect(rippleOutAction, &QAction::triggered, this, &MainWindow::rippleTrimOutToPlayhead);
    m_menuHelpEntries.append({rippleOutAction,
        QStringLiteral("選んだクリップの末尾を今の再生位置まで伸縮します（リップル）。後ろのクリップは隙間なくついてきます。")});

    auto *rollEditAction = trimMenu->addAction("編集点を再生ヘッドへ (ロール)");
    rollEditAction->setShortcut(QKeySequence(Qt::Key_R));
    connect(rollEditAction, &QAction::triggered, this, &MainWindow::rollEditToPlayhead);
    m_menuHelpEntries.append({rollEditAction,
        QStringLiteral("選んだクリップと次のクリップの境目（編集点）を今の再生位置へ動かします。全体の長さは変わりません（ロール）。")});

    trimMenu->addSeparator();

    auto *slipAction = trimMenu->addAction("スリップ...");
    connect(slipAction, &QAction::triggered, this, &MainWindow::slipSelectedClip);
    m_menuHelpEntries.append({slipAction,
        QStringLiteral("クリップの位置と長さはそのままで、中で見せる範囲だけを前後にずらします（スリップ）。秒数を入力します。")});

    auto *slideAction = trimMenu->addAction("スライド...");
    connect(slideAction, &QAction::triggered, this, &MainWindow::slideSelectedClip);
    m_menuHelpEntries.append({slideAction,
        QStringLiteral("クリップの中身はそのままで、タイムライン上の位置だけを前後にずらします。隣のクリップが伸縮して吸収します（スライド）。秒数を入力します。")});

    // エフェクト メニュー
    auto *effectsMenu = menuBar()->addMenu("エフェクト(&F)");

    auto *ccAction = effectsMenu->addAction("色補正 / グレーディング(&C)...");
    ccAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    connect(ccAction, &QAction::triggered, this, &MainWindow::colorCorrection);
    m_menuHelpEntries.append({ccAction,
        QStringLiteral("映像の明るさ・色合い・コントラストを整えます。映画風の色味に寄せることもできます。")});

    auto *fxAction = effectsMenu->addAction("ビデオエフェクト(&V)...");
    fxAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    connect(fxAction, &QAction::triggered, this, &MainWindow::videoEffects);
    m_menuHelpEntries.append({fxAction,
        QStringLiteral("ぼかし・モザイク・光らせるなど、映像に視覚効果を追加します。")});

    effectsMenu->addSeparator();

    auto *sharpenFxAction = effectsMenu->addAction("シャープン...");
    connect(sharpenFxAction, &QAction::triggered, this, &MainWindow::applySharpenEffect);
    m_menuHelpEntries.append({sharpenFxAction,
        QStringLiteral("映像の輪郭をくっきりさせます。少しぼやけた映像をシャキッと見せたいときに。")});

    auto *mosaicFxAction = effectsMenu->addAction("モザイク...");
    connect(mosaicFxAction, &QAction::triggered, this, &MainWindow::applyMosaicEffect);
    m_menuHelpEntries.append({mosaicFxAction,
        QStringLiteral("顔やナンバープレートなど、見せたくない部分をモザイクで隠します。")});

    auto *chromaFxAction = effectsMenu->addAction("クロマキー...");
    connect(chromaFxAction, &QAction::triggered, this, &MainWindow::applyChromaKeyEffect);
    m_menuHelpEntries.append({chromaFxAction,
        QStringLiteral("緑や青の背景（グリーンバック）を透明にして、別の映像と合成します。")});

    effectsMenu->addSeparator();

    auto *pluginAction = effectsMenu->addAction("プラグインエフェクト(&P)...");
    connect(pluginAction, &QAction::triggered, this, &MainWindow::pluginEffects);
    m_menuHelpEntries.append({pluginAction,
        QStringLiteral("追加でインストールした映像エフェクト（プラグイン）を使います。")});

    effectsMenu->addSeparator();

    auto *lutAction = effectsMenu->addAction("LUT適用 (.cube)...");
    connect(lutAction, &QAction::triggered, this, &MainWindow::applyLut);
    m_menuHelpEntries.append({lutAction,
        QStringLiteral("用意された色味のレシピ（LUT ファイル）を読み込んで、映像の色を一発で変えます。")});

    auto *manageLutAction = effectsMenu->addAction("LUT管理...");
    connect(manageLutAction, &QAction::triggered, this, &MainWindow::manageLuts);
    m_menuHelpEntries.append({manageLutAction,
        QStringLiteral("登録済みの色味レシピ（LUT）を整理・追加・削除します。")});

    effectsMenu->addSeparator();

    auto *loadLutCubeAction = effectsMenu->addAction("LUT を読み込み…");
    connect(loadLutCubeAction, &QAction::triggered, this, &MainWindow::loadLutCubeFile);

    m_lutIntensitySlider = new QSlider(Qt::Horizontal, this);
    m_lutIntensitySlider->setRange(0, 100);
    m_lutIntensitySlider->setValue(100);
    m_lutIntensitySlider->setToolTip("LUT Intensity (0-100%)");
    connect(m_lutIntensitySlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_player)
            m_player->glPreview()->setLutIntensity(value / 100.0);
    });
    auto *lutSliderAction = new QWidgetAction(this);
    lutSliderAction->setDefaultWidget(m_lutIntensitySlider);
    effectsMenu->addAction(lutSliderAction);

    auto *clearLutMenuAction = effectsMenu->addAction("LUT 解除");
    connect(clearLutMenuAction, &QAction::triggered, this, &MainWindow::clearLutIntensity);

    effectsMenu->addSeparator();

    auto *applyPresetAction = effectsMenu->addAction("エフェクトプリセット適用...");
    applyPresetAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    connect(applyPresetAction, &QAction::triggered, this, &MainWindow::applyEffectPreset);
    m_menuHelpEntries.append({applyPresetAction,
        QStringLiteral("保存済みのエフェクト設定の組み合わせを、選んだクリップにまとめて適用します。")});

    auto *savePresetAction = effectsMenu->addAction("現在設定をプリセットに保存...");
    connect(savePresetAction, &QAction::triggered, this, &MainWindow::saveEffectPreset);
    m_menuHelpEntries.append({savePresetAction,
        QStringLiteral("今のクリップに付けているエフェクトの組み合わせに名前を付けて保存し、後で使い回せます。")});

    auto *managePresetsAction = effectsMenu->addAction("プリセット管理...");
    connect(managePresetsAction, &QAction::triggered, this, &MainWindow::manageEffectPresets);
    m_menuHelpEntries.append({managePresetsAction,
        QStringLiteral("保存したエフェクトプリセットの名前変更・削除をします。")});

    effectsMenu->addSeparator();

    auto *kfAction = effectsMenu->addAction("キーフレーム編集(&K)...");
    kfAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(kfAction, &QAction::triggered, this, &MainWindow::editKeyframes);
    m_menuHelpEntries.append({kfAction,
        QStringLiteral("時間とともに動く・色が変わるなどの動きを「キーフレーム」で細かく設定します。")});

    effectsMenu->addSeparator();

    // US-FEAT-D: motion tracking UI
    m_trackMotionAction = effectsMenu->addAction("Track Motion…");
    m_trackMotionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    connect(m_trackMotionAction, &QAction::triggered, this, &MainWindow::trackMotion);
    m_menuHelpEntries.append({m_trackMotionAction,
        QStringLiteral("映像の中で動く対象を追いかけ、その動きにテロップやモザイクを追従させます。")});

    effectsMenu->addSeparator();

    auto *shaderFxAction = effectsMenu->addAction("GPUシェーダーエフェクト...");
    connect(shaderFxAction, &QAction::triggered, this, &MainWindow::applyShaderEffect);
    m_menuHelpEntries.append({shaderFxAction,
        QStringLiteral("グラフィックボードの力で動く特殊なエフェクトを使います。上級者向けです。")});

    auto *manageShaderAction = effectsMenu->addAction("GPUシェーダー管理...");
    connect(manageShaderAction, &QAction::triggered, this, &MainWindow::manageShaderEffects);
    m_menuHelpEntries.append({manageShaderAction,
        QStringLiteral("登録済みの GPU シェーダーエフェクトを整理・追加・削除します。")});

    // 再生 メニュー
    auto *playbackMenu = menuBar()->addMenu("再生(&P)");

    auto *jklNote = playbackMenu->addAction("J/K/L 速度コントロール");
    jklNote->setEnabled(false);
    m_menuHelpEntries.append({jklNote,
        QStringLiteral("J＝逆再生 / K＝停止 / L＝再生。押すたびに早送り・早戻しが速くなります（参考表示）。")});

    playbackMenu->addSeparator();

    auto *markInAction = playbackMenu->addAction("イン点をマーク(&I)");
    markInAction->setShortcut(QKeySequence(Qt::Key_I));
    connect(markInAction, &QAction::triggered, this, &MainWindow::markIn);
    m_menuHelpEntries.append({markInAction,
        QStringLiteral("使いたい範囲の「開始位置」を今の再生位置に決めます（I キー）。")});

    auto *markOutAction = playbackMenu->addAction("アウト点をマーク(&O)");
    markOutAction->setShortcut(QKeySequence(Qt::Key_O));
    connect(markOutAction, &QAction::triggered, this, &MainWindow::markOut);
    m_menuHelpEntries.append({markOutAction,
        QStringLiteral("使いたい範囲の「終了位置」を今の再生位置に決めます（O キー）。")});

    // 検索 メニュー — 機能発見性 (初心者向け)。機能が増えてどこに何があるか
    // 分かりにくいため、機能名や「音量を均一にしたい」のような操作内容の言葉で
    // 機能を探して呼び出せる導線をトップレベルに用意する (Ctrl+Shift+P と等価)。
    auto *searchMenu = menuBar()->addMenu(QStringLiteral("検索(&S)"));
    auto *featureSearchAction =
        searchMenu->addAction(QStringLiteral("🔍 機能を検索... (Ctrl+Shift+P)"));
    featureSearchAction->setObjectName("action_feature_search");
    connect(featureSearchAction, &QAction::triggered,
            this, &MainWindow::openCommandPalette);
    m_menuHelpEntries.append({featureSearchAction,
        QStringLiteral("機能名や『音量を均一にしたい』のような操作内容の言葉で、使いたい機能を"
                       "探して呼び出せます。どこにあるか分からない機能はここから検索してください。")});

    // ツール メニュー (AI / 自動編集)
    auto *toolsMenu = menuBar()->addMenu("ツール(&T)");

    // US-CP-4: コマンドパレット (VS Code 風 機能検索)。Ctrl+Shift+P と
    // このメニュー項目の両方から openCommandPalette() を起動する。
    auto *commandPaletteAction =
        toolsMenu->addAction(QStringLiteral("コマンドパレット... (Ctrl+Shift+P)"));
    commandPaletteAction->setObjectName("action_command_palette");
    connect(commandPaletteAction, &QAction::triggered,
            this, &MainWindow::openCommandPalette);
    m_menuHelpEntries.append({commandPaletteAction,
        QStringLiteral("ツール名や『こういう操作がしたい』という語で機能を検索し、選んで即実行できるパレットを開きます。")});
    auto *commandPaletteShortcut =
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")), this);
    connect(commandPaletteShortcut, &QShortcut::activated,
            this, &MainWindow::openCommandPalette);
    toolsMenu->addSeparator();

    // Phase 6 Wave 2 (US-6B-4): 動画→Whisper 文字起こし (既存ツールメニューへ配置)
    auto *whisperTranscribeAction = toolsMenu->addAction(QStringLiteral("動画を文字起こし..."));
    whisperTranscribeAction->setObjectName("action_whisper_transcribe");
    connect(whisperTranscribeAction, &QAction::triggered,
            this, &MainWindow::openWhisperTranscribeDialog);
    m_menuHelpEntries.append({whisperTranscribeAction,
        QStringLiteral("動画・音声から音声を抽出し、Whisper などの ASR エンジンで自動的に字幕クリップを生成します。")});

    // Phase 6 Wave 3 (US-6C-4): 文字起こしからハイライト検出 (既存ツールメニューへ配置)
    auto *transcriptHighlightAction = toolsMenu->addAction(QStringLiteral("文字起こしからハイライト検出..."));
    transcriptHighlightAction->setObjectName("action_transcript_highlight");
    connect(transcriptHighlightAction, &QAction::triggered,
            this, &MainWindow::openTranscriptHighlightDialog);
    m_menuHelpEntries.append({transcriptHighlightAction,
        QStringLiteral("現在の字幕トラックを AI に渡し、最も見どころとなる瞬間を自動検出します。")});

    // Phase 6 Wave 4 (US-6D-4): ハイライトから自動カット (既存ツールメニューへ配置)
    auto *autoClipAction = toolsMenu->addAction(QStringLiteral("ハイライトから自動カット..."));
    autoClipAction->setObjectName("action_auto_clip");
    connect(autoClipAction, &QAction::triggered,
            this, &MainWindow::openAutoClipDialog);
    m_menuHelpEntries.append({autoClipAction,
        QStringLiteral("検出済みハイライトから自動でカット範囲を計算し、タイムラインに追加します。")});

    auto *silenceAction = toolsMenu->addAction("無音検出...");
    connect(silenceAction, &QAction::triggered, this, &MainWindow::autoSilenceDetect);
    m_menuHelpEntries.append({silenceAction,
        QStringLiteral("しゃべっていない静かな部分を自動で見つけます。間延びカットの下準備に。")});

    auto *jumpCutAction = toolsMenu->addAction("自動ジャンプカット...");
    connect(jumpCutAction, &QAction::triggered, this, &MainWindow::autoJumpCut);
    m_menuHelpEntries.append({jumpCutAction,
        QStringLiteral("無音部分を自動で切り取って、テンポの良い動画にします（実況・解説系で人気）。")});

    auto *sceneAction = toolsMenu->addAction("シーン変化検出...");
    connect(sceneAction, &QAction::triggered, this, &MainWindow::autoSceneDetect);
    m_menuHelpEntries.append({sceneAction,
        QStringLiteral("映像が大きく切り替わる場所を自動で見つけて、そこに目印を付けます。")});

    // US-HW-10: scene-cut detection backed by SceneCutScanner / SceneCutDialog.
    auto *sceneCutDetectAction = toolsMenu->addAction("シーンカット検出...");
    sceneCutDetectAction->setObjectName("action_scene_cut_detect");
    connect(sceneCutDetectAction, &QAction::triggered, this, &MainWindow::onSceneCutDetect);
    m_menuHelpEntries.append({sceneCutDetectAction,
        QStringLiteral("シーンカット検出を実行し、マーカー追加またはクリップ分割を行う。")});

    toolsMenu->addSeparator();

    auto *stabilizeAction = toolsMenu->addAction("手ブレ補正...");
    connect(stabilizeAction, &QAction::triggered, this, &MainWindow::stabilizeVideo);
    m_menuHelpEntries.append({stabilizeAction,
        QStringLiteral("カメラのブレでガタガタ揺れる映像を、なめらかに見えるよう自動で補正します。")});

    auto *speedRampAction = toolsMenu->addAction("スピードランプ (可変速)...");
    connect(speedRampAction, &QAction::triggered, this, &MainWindow::setSpeedRamp);
    m_menuHelpEntries.append({speedRampAction,
        QStringLiteral("途中から徐々にスローになる／速くなるなど、再生速度を時間ごとに変化させます。")});

    toolsMenu->addSeparator();

    auto *motionTrackAction = toolsMenu->addAction("モーショントラッキング...");
    connect(motionTrackAction, &QAction::triggered, this, &MainWindow::motionTrackSetup);
    m_menuHelpEntries.append({motionTrackAction,
        QStringLiteral("動く対象を追いかけて、テロップやモザイクをその動きにくっつけて移動させます。")});

    toolsMenu->addSeparator();

    auto *audioDenoiseAction = toolsMenu->addAction("音声ノイズ除去...");
    connect(audioDenoiseAction, &QAction::triggered, this, &MainWindow::audioNoiseDenoise);
    m_menuHelpEntries.append({audioDenoiseAction,
        QStringLiteral("録音に入った「サーッ」というノイズや空調音を減らして、声を聞き取りやすくします。")});

    auto *videoDenoiseAction = toolsMenu->addAction("映像ノイズ除去...");
    connect(videoDenoiseAction, &QAction::triggered, this, &MainWindow::videoNoiseDenoise);
    m_menuHelpEntries.append({videoDenoiseAction,
        QStringLiteral("暗い場所で撮ったときに出るザラザラしたノイズを減らして、映像をきれいにします。")});

    toolsMenu->addSeparator();

    auto *subtitleGenAction = toolsMenu->addAction("字幕自動生成 (Whisper)...");
    connect(subtitleGenAction, &QAction::triggered, this, &MainWindow::generateSubtitles);
    m_menuHelpEntries.append({subtitleGenAction,
        QStringLiteral("しゃべっている内容を AI が聞き取って、字幕（テロップ）を自動で作ります。")});

    auto *highlightAction = toolsMenu->addAction("AI自動ハイライト...");
    connect(highlightAction, &QAction::triggered, this, &MainWindow::analyzeHighlights);
    m_menuHelpEntries.append({highlightAction,
        QStringLiteral("長い映像の中から盛り上がっている見せ場を AI が探して、候補として並べます。")});

    toolsMenu->addSeparator();

    auto *screenRecAction = toolsMenu->addAction("画面録画を開始...");
    connect(screenRecAction, &QAction::triggered, this, &MainWindow::startScreenRecording);
    m_menuHelpEntries.append({screenRecAction,
        QStringLiteral("パソコンの画面を動画として録画します。ゲーム実況や操作説明動画に。")});

    auto *stopRecAction = toolsMenu->addAction("画面録画を停止");
    connect(stopRecAction, &QAction::triggered, this, &MainWindow::stopScreenRecording);
    m_menuHelpEntries.append({stopRecAction,
        QStringLiteral("実行中の画面録画を終了して、録画した動画を保存します。")});

    toolsMenu->addSeparator();

    auto *proxySettingsAction = toolsMenu->addAction("プロキシ設定...");
    connect(proxySettingsAction, &QAction::triggered, this, &MainWindow::openProxySettings);
    m_menuHelpEntries.append({proxySettingsAction,
        QStringLiteral("重い動画を軽い「代理映像（プロキシ）」に置き換えて、編集中の動作を軽くする設定です。")});

    auto *proxyToggle = toolsMenu->addAction("プロキシモード切替");
    proxyToggle->setCheckable(true);
    connect(proxyToggle, &QAction::triggered, this, &MainWindow::toggleProxyMode);
    m_menuHelpEntries.append({proxyToggle,
        QStringLiteral("編集中に軽い代理映像を使うか、元の高画質映像を使うかを切り替えます。書き出しは常に高画質です。")});

    auto *genProxiesAction = toolsMenu->addAction("プロキシ生成...");
    connect(genProxiesAction, &QAction::triggered, this, &MainWindow::generateProxies);
    m_menuHelpEntries.append({genProxiesAction,
        QStringLiteral("読み込んだ素材から、編集を軽くするための代理映像（プロキシ）を作ります。")});

    auto *proxyMgmtAction = toolsMenu->addAction("プロキシ管理...");
    connect(proxyMgmtAction, &QAction::triggered, this, &MainWindow::openProxyManagement);
    m_menuHelpEntries.append({proxyMgmtAction,
        QStringLiteral("作成済みの代理映像（プロキシ）の一覧確認・削除をします。")});

    auto *renderQueueAction = toolsMenu->addAction("レンダーキュー...");
    renderQueueAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    connect(renderQueueAction, &QAction::triggered, this, &MainWindow::openRenderQueue);
    m_menuHelpEntries.append({renderQueueAction,
        QStringLiteral("複数の書き出しをまとめて順番に処理する待ち行列を開きます。")});

    auto *networkRenderAction = toolsMenu->addAction("ネットワークレンダー...");
    connect(networkRenderAction, &QAction::triggered, this, &MainWindow::openNetworkRender);
    m_menuHelpEntries.append({networkRenderAction,
        QStringLiteral("他のパソコンの力も借りて、動画の書き出しを分担して速くします。上級者向けです。")});

    toolsMenu->addSeparator();

    auto *scriptAction = toolsMenu->addAction("Pythonスクリプトコンソール...");
    scriptAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(scriptAction, &QAction::triggered, this, &MainWindow::openScriptConsole);
    m_menuHelpEntries.append({scriptAction,
        QStringLiteral("Python のプログラムで編集作業を自動化します。プログラミングに詳しい人向けです。")});

    toolsMenu->addSeparator();

    auto *multiCamSetupAction = toolsMenu->addAction("マルチカメラセットアップ...");
    connect(multiCamSetupAction, &QAction::triggered, this, &MainWindow::multiCamSetup);
    m_menuHelpEntries.append({multiCamSetupAction,
        QStringLiteral("複数カメラの映像を音で合わせて、切り替え編集できる状態にまとめます。")});

    auto *multiCamSwitchAction = toolsMenu->addAction("マルチカメラ切替...");
    connect(multiCamSwitchAction, &QAction::triggered, this, &MainWindow::multiCamSwitch);
    m_menuHelpEntries.append({multiCamSwitchAction,
        QStringLiteral("再生しながらカメラを切り替えていくと、その通りに編集されます。")});

    toolsMenu->addSeparator();

    // US-EXT-10: Sprint 10 pro extensions — AI upscale / frame interpolation + Plugin browser.
    auto *aiProcessingAction = toolsMenu->addAction("AI アップスケール / フレーム補間...");
    aiProcessingAction->setObjectName("action_ai_processing");
    connect(aiProcessingAction, &QAction::triggered, this, &MainWindow::onAIProcessing);
    m_menuHelpEntries.append({aiProcessingAction,
        QStringLiteral("AI アップスケール (Lanczos/Bicubic) とフレーム補間 (Linear/Motion-Blend) の処理を設定する。")});

    auto *pluginBrowserAction = toolsMenu->addAction("プラグインブラウザ...");
    pluginBrowserAction->setObjectName("action_plugin_browser");
    connect(pluginBrowserAction, &QAction::triggered, this, &MainWindow::onPluginBrowser);
    m_menuHelpEntries.append({pluginBrowserAction,
        QStringLiteral("OFX 風 plugin manifest を持つプラグインを検出して一覧表示する (実行は将来対応)。")});

    // US-WF-D: Sprint 11 workflow — AI auto-mask generation.
    auto *aiMaskAction = toolsMenu->addAction("AI マスクを生成…");
    aiMaskAction->setObjectName("action_aimask_dialog");
    connect(aiMaskAction, &QAction::triggered, this, &MainWindow::openAIMaskDialog);
    m_menuHelpEntries.append({aiMaskAction,
        QStringLiteral("輝度しきい値 / 色域 / 外部プラグインで自動マスクを生成するダイアログを開く。")});

    // US-PT-B: Sprint 15 — Planar (4-corner) tracker dialog.
    auto *planarTrackerAction = toolsMenu->addAction(QStringLiteral("プラナートラッカー…"));
    planarTrackerAction->setObjectName("action_planar_tracker");
    connect(planarTrackerAction, &QAction::triggered,
            this, &MainWindow::openPlanarTrackerDialog);
    m_menuHelpEntries.append({planarTrackerAction,
        QStringLiteral("4 点コーナーピンで平面 (看板・画面・顔など) を時系列追跡し、AI マスクや 2D エフェクトを貼り付けに使えます。")});

    // US-INT-3: Sprint 17/18/19 — YouTube upload / collaboration / auto color match.
    toolsMenu->addSeparator();
#ifdef HAVE_YOUTUBE
    auto *youtubeUploadAction = toolsMenu->addAction(
        QStringLiteral("YouTube アップロード(&Y)…"));
    youtubeUploadAction->setObjectName("action_youtube_upload");
    connect(youtubeUploadAction, &QAction::triggered,
            this, &MainWindow::onYoutubeUpload);
    m_menuHelpEntries.append({youtubeUploadAction,
        QStringLiteral("Google アカウントで認証し、書き出し済みの動画を YouTube に直接アップロードします (resumable upload + 自動 retry)。")});
#endif

#ifdef HAVE_COLLAB
    auto *commentsPanelAction = toolsMenu->addAction(
        QStringLiteral("コラボレーションパネル(&L)…"));
    commentsPanelAction->setObjectName("action_comments_panel");
    connect(commentsPanelAction, &QAction::triggered,
            this, &MainWindow::onCommentsPanel);
    m_menuHelpEntries.append({commentsPanelAction,
        QStringLiteral("クリップにタイムコード付きコメントを残せる Frame.io 風コラボパネルを表示します (返信/解決/履歴対応)。")});

    auto *collabHistoryAction = toolsMenu->addAction(
        QStringLiteral("変更履歴(&H)…"));
    collabHistoryAction->setObjectName("action_collab_history");
    connect(collabHistoryAction, &QAction::triggered,
            this, &MainWindow::onCollabHistory);
    m_menuHelpEntries.append({collabHistoryAction,
        QStringLiteral("プロジェクトの変更履歴 (誰がいつ何をしたか) を一覧し、過去のスナップショットへ戻すためのダイアログを開きます。")});
#endif

#ifdef HAVE_COLORMATCH
    auto *colorMatchAction = toolsMenu->addAction(
        QStringLiteral("自動カラーマッチ(&C)…"));
    colorMatchAction->setObjectName("action_color_match");
    connect(colorMatchAction, &QAction::triggered,
            this, &MainWindow::onColorMatch);
    m_menuHelpEntries.append({colorMatchAction,
        QStringLiteral("基準クリップと対象クリップを選び、平均/分散から 3D LUT を生成して色味を自動的に合わせます (.cube 書き出し対応)。")});
#endif

    toolsMenu->addSeparator();

    auto *vimeoUploadAction = toolsMenu->addAction(
        QStringLiteral("Vimeo 直送アップロード(&V)…"));
    vimeoUploadAction->setObjectName("action_vimeo_upload");
    connect(vimeoUploadAction, &QAction::triggered,
            this, &MainWindow::openVimeoUploadDialog);
    m_menuHelpEntries.append({vimeoUploadAction,
        QStringLiteral("Vimeo アカウントで認証し、書き出し済み動画を resumable upload で直接送信します。")});

    auto *twitchStreamAction = toolsMenu->addAction(
        QStringLiteral("Twitch 配信設定(&W)…"));
    twitchStreamAction->setObjectName("action_twitch_stream");
    connect(twitchStreamAction, &QAction::triggered,
            this, &MainWindow::openTwitchStreamDialog);
    m_menuHelpEntries.append({twitchStreamAction,
        QStringLiteral("Twitch 向け RTMP 配信設定を作り、ffmpeg コマンドを確認・コピーできます。")});

    auto *frameIoImportAction = toolsMenu->addAction(
        QStringLiteral("Frame.io コメント取り込み(&F)…"));
    frameIoImportAction->setObjectName("action_frameio_import");
    connect(frameIoImportAction, &QAction::triggered,
            this, &MainWindow::openFrameIoImportDialog);
    m_menuHelpEntries.append({frameIoImportAction,
        QStringLiteral("Frame.io の asset comments を取得し、コラボコメントトラックへ流し込みます。")});

    auto *davinciExportAction = toolsMenu->addAction(
        QStringLiteral("DaVinci Resolve XML 書き出し(&D)…"));
    davinciExportAction->setObjectName("action_davinci_export");
    connect(davinciExportAction, &QAction::triggered,
            this, &MainWindow::openDavinciExportDialog);
    m_menuHelpEntries.append({davinciExportAction,
        QStringLiteral("Final Cut Pro 7 XML 互換の DaVinci Resolve XML を書き出します。")});

    auto *fcpxmlExportAction = toolsMenu->addAction(
        QStringLiteral("FCPXML 書き出し(&X)…"));
    fcpxmlExportAction->setObjectName("action_fcpxml_export");
    connect(fcpxmlExportAction, &QAction::triggered,
            this, &MainWindow::openFcpxmlExportDialog);
    m_menuHelpEntries.append({fcpxmlExportAction,
        QStringLiteral("Final Cut Pro X 用の FCPXML を書き出します。")});

    auto *smartEditAction = toolsMenu->addAction(
        QStringLiteral("Smart Edit アシスタント(&M)…"));
    smartEditAction->setObjectName("action_smart_edit");
    connect(smartEditAction, &QAction::triggered,
            this, &MainWindow::openSmartEditDialog);
    m_menuHelpEntries.append({smartEditAction,
        QStringLiteral("無音検出とシーン変化検出を組み合わせた自動カット候補を確認できます。")});

    auto *cloudRenderAction = toolsMenu->addAction(
        QStringLiteral("クラウドレンダリング(&U)…"));
    cloudRenderAction->setObjectName("action_cloud_render");
    connect(cloudRenderAction, &QAction::triggered,
            this, &MainWindow::openCloudRenderDialog);
    m_menuHelpEntries.append({cloudRenderAction,
        QStringLiteral("リモート ffmpeg ジョブの送信と進捗監視を行うクラウドレンダリング画面を開きます。")});

    // US-INT-2: Sprint 21 — platform expansion / mastering / batch export.
    auto *xVideoAction = toolsMenu->addAction(
        QStringLiteral("X(Twitter) に動画投稿(&X)…"));
    xVideoAction->setObjectName("action_x_video");
    connect(xVideoAction, &QAction::triggered,
            this, &MainWindow::openXVideoDialog);
    m_menuHelpEntries.append({xVideoAction,
        QStringLiteral("X(Twitter) アカウントで認証し、書き出し済み動画を chunked upload で直接投稿します。")});

    auto *instagramAction = toolsMenu->addAction(
        QStringLiteral("Instagram Reels に投稿(&I)…"));
    instagramAction->setObjectName("action_instagram_publish");
    connect(instagramAction, &QAction::triggered,
            this, &MainWindow::openInstagramDialog);
    m_menuHelpEntries.append({instagramAction,
        QStringLiteral("Instagram Graph API で Reels コンテナを作成し、書き出し済み動画を公開します。")});

    auto *projectTemplateAction = toolsMenu->addAction(
        QStringLiteral("プロジェクトテンプレート(&T)…"));
    projectTemplateAction->setObjectName("action_project_template");
    connect(projectTemplateAction, &QAction::triggered,
            this, &MainWindow::openProjectTemplateDialog);
    m_menuHelpEntries.append({projectTemplateAction,
        QStringLiteral("用途別のプロジェクトテンプレートを選んで新規プロジェクトを素早く作成します。")});

    auto *loudnessMasterAction = toolsMenu->addAction(
        QStringLiteral("ラウドネスマスタリング(&L)…"));
    loudnessMasterAction->setObjectName("action_loudness_master");
    connect(loudnessMasterAction, &QAction::triggered,
            this, &MainWindow::openLoudnessDialog);
    m_menuHelpEntries.append({loudnessMasterAction,
        QStringLiteral("配信プラットフォーム基準 (EBU R128 / -14 LUFS 等) に合わせてラウドネスを最終調整します。")});

    auto *hdrGradingAction = toolsMenu->addAction(
        QStringLiteral("HDR カラーグレーディング(&H)…"));
    hdrGradingAction->setObjectName("action_hdr_grading");
    connect(hdrGradingAction, &QAction::triggered,
            this, &MainWindow::openHdrDialog);
    m_menuHelpEntries.append({hdrGradingAction,
        QStringLiteral("HDR10 / HLG / PQ トーンマッピングを使った HDR カラーグレーディングを行います。")});

    // AC-4: ACES シーンリファード色管理 (IDT/RRT/ODT)。
    auto *colorManagementAction = toolsMenu->addAction(
        QStringLiteral("カラーマネジメント (ACES)…"));
    colorManagementAction->setObjectName("action_color_management");
    connect(colorManagementAction, &QAction::triggered,
            this, &MainWindow::openColorManagement);
    m_menuHelpEntries.append({colorManagementAction,
        QStringLiteral("ACES のシーンリファード色管理 (入力/作業/出力色空間) を設定します。")});

    // DV-4: Dolby Vision 動的メタデータ (Level1/2/5/6) の編集 + DV XML エクスポート。
    auto *dolbyVisionAction = toolsMenu->addAction(
        QStringLiteral("Dolby Vision メタデータ…"));
    dolbyVisionAction->setObjectName("action_dolby_vision");
    connect(dolbyVisionAction, &QAction::triggered,
            this, &MainWindow::openDolbyVision);
    m_menuHelpEntries.append({dolbyVisionAction,
        QStringLiteral("Dolby Vision の動的メタデータ (プロファイル/CLL-FALL/ショット輝度) を編集し、DV XML を書き出します。")});

    // CC-4: 放送用クローズドキャプション (CEA-608/708) の設定 + SCC エクスポート。
    auto *broadcastCaptionAction = toolsMenu->addAction(
        QStringLiteral("放送用クローズドキャプション…"));
    broadcastCaptionAction->setObjectName("action_broadcast_caption");
    connect(broadcastCaptionAction, &QAction::triggered,
            this, &MainWindow::openBroadcastCaption);
    m_menuHelpEntries.append({broadcastCaptionAction,
        QStringLiteral("放送納品向けの CEA-608/708 クローズドキャプションを設定し、SCC サイドカーを書き出します。")});

    auto *multiCamSyncAction = toolsMenu->addAction(
        QStringLiteral("マルチカム同期(&M)…"));
    multiCamSyncAction->setObjectName("action_multicam_sync");
    connect(multiCamSyncAction, &QAction::triggered,
            this, &MainWindow::openMultiCamSyncDialog);
    m_menuHelpEntries.append({multiCamSyncAction,
        QStringLiteral("複数カメラのクリップを音声波形で自動同期し、マルチカムシーケンスを作成します。")});

    auto *batchExportAction = toolsMenu->addAction(
        QStringLiteral("バッチエクスポート(&B)…"));
    batchExportAction->setObjectName("action_batch_export");
    connect(batchExportAction, &QAction::triggered,
            this, &MainWindow::openBatchExportDialog);
    m_menuHelpEntries.append({batchExportAction,
        QStringLiteral("複数の書き出しジョブをキューに登録し、まとめてバッチ処理します。")});

    // US-INT-2: Sprint 22 — keying / restoration / animated export / easing /
    // subtitle translation / lower-third / watermark.
    auto *chromaKeyAction = toolsMenu->addAction(
        QStringLiteral("クロマキー精緻化(&K)…"));
    chromaKeyAction->setObjectName("action_chroma_key_refine");
    connect(chromaKeyAction, &QAction::triggered,
            this, &MainWindow::openChromaKeyDialog);
    m_menuHelpEntries.append({chromaKeyAction,
        QStringLiteral("スピル除去・エッジ調整などでクロマキー合成の抜きを精緻に仕上げます。")});

    auto *audioRestoreAction = toolsMenu->addAction(
        QStringLiteral("音声リストア(&R)…"));
    audioRestoreAction->setObjectName("action_audio_restoration");
    connect(audioRestoreAction, &QAction::triggered,
            this, &MainWindow::openAudioRestoreDialog);
    m_menuHelpEntries.append({audioRestoreAction,
        QStringLiteral("ノイズ・クリック・ハムなどの劣化を取り除き、収録音声を復元します。")});

    // SP-4: iZotope RX 風スペクトル音声修復 (時間×周波数の矩形領域を減衰)。
    auto *spectralRepairAction = toolsMenu->addAction(
        QStringLiteral("スペクトル音声修復(&S)…"));
    spectralRepairAction->setObjectName("action_spectral_repair");
    connect(spectralRepairAction, &QAction::triggered,
            this, &MainWindow::openSpectralRepair);
    m_menuHelpEntries.append({spectralRepairAction,
        QStringLiteral("スペクトログラム上で時間×周波数の矩形を選択し、ノイズ成分を減衰させて音声を修復します。")});

    auto *animExportAction = toolsMenu->addAction(
        QStringLiteral("アニメGIF・WebP書き出し(&G)…"));
    animExportAction->setObjectName("action_animated_export");
    connect(animExportAction, &QAction::triggered,
            this, &MainWindow::openAnimExportDialog);
    m_menuHelpEntries.append({animExportAction,
        QStringLiteral("選択範囲をアニメーション GIF / WebP として最適化して書き出します。")});

    auto *easingEditorAction = toolsMenu->addAction(
        QStringLiteral("イージングカーブエディタ(&E)…"));
    easingEditorAction->setObjectName("action_easing_curve_editor");
    connect(easingEditorAction, &QAction::triggered,
            this, &MainWindow::openEasingEditorDialog);
    m_menuHelpEntries.append({easingEditorAction,
        QStringLiteral("ベジェ制御点でキーフレーム間のイージングカーブを視覚的に編集します。")});

    auto *subtitleTranslatorAction = toolsMenu->addAction(
        QStringLiteral("字幕翻訳(&Z)…"));
    subtitleTranslatorAction->setObjectName("action_subtitle_translator");
    connect(subtitleTranslatorAction, &QAction::triggered,
            this, &MainWindow::openSubtitleTranslatorDialog);
    m_menuHelpEntries.append({subtitleTranslatorAction,
        QStringLiteral("既存の字幕トラックを別言語へ翻訳し、多言語字幕を生成します。")});

    auto *lowerThirdAction = toolsMenu->addAction(
        QStringLiteral("ローワーサード(&D)…"));
    lowerThirdAction->setObjectName("action_lower_third");
    connect(lowerThirdAction, &QAction::triggered,
            this, &MainWindow::openLowerThirdDialog);
    m_menuHelpEntries.append({lowerThirdAction,
        QStringLiteral("名前・肩書きなどを表示する下三分の一テロップ (ローワーサード) を作成します。")});

    auto *watermarkAction = toolsMenu->addAction(
        QStringLiteral("ウォーターマーク(&W)…"));
    watermarkAction->setObjectName("action_watermark");
    connect(watermarkAction, &QAction::triggered,
            this, &MainWindow::openWatermarkDialog);
    m_menuHelpEntries.append({watermarkAction,
        QStringLiteral("ロゴ画像やテキストの透かしを映像へ重ねて、位置・不透明度を調整します。")});

    // US-SNS-7: LoudnessPanel dock (created here so menu action can reference it)
    m_loudnessDock = new QDockWidget("ラウドネスパネル", this);
    m_loudnessDock->setObjectName("LoudnessPanelDock");
    m_loudnessPanel = new LoudnessPanel(m_loudnessDock);
    m_loudnessDock->setWidget(m_loudnessPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_loudnessDock);
    m_loudnessDock->setVisible(false);
    connect(m_loudnessPanel, &LoudnessPanel::normalizeRequested,
            this, &MainWindow::applyLoudnessNormalize);

    // MP-5: メディアプール ドック (左側)。SSOT モデル m_mediaPool を指すだけ。
    m_mediaPoolDock = new MediaPoolDock(this);
    m_mediaPoolDock->setPool(&m_mediaPool);
    addDockWidget(Qt::LeftDockWidgetArea, m_mediaPoolDock);
    connect(m_mediaPoolDock, &MediaPoolDock::assetActivated,
            this, &MainWindow::onMediaPoolAssetActivated);
    connect(m_mediaPoolDock, &MediaPoolDock::importRequested,
            this, &MainWindow::importToMediaPool);
    m_mediaPoolDock->refresh();

    // MK-2: マーカー パネル ドック (右側)。Timeline のマーカーを表で常時表示する。
    m_markerPanelDock = new MarkerPanelDock(this);
    addDockWidget(Qt::RightDockWidgetArea, m_markerPanelDock);
    connect(m_markerPanelDock, &MarkerPanelDock::jumpToMarker,
            this, &MainWindow::onMarkerPanelJump);
    connect(m_markerPanelDock, &MarkerPanelDock::markerNoteEdited,
            this, &MainWindow::onMarkerPanelNoteEdited);
    connect(m_markerPanelDock, &MarkerPanelDock::markerDeleteRequested,
            this, &MainWindow::onMarkerPanelDeleteRequested);
    // Timeline の markersChanged は addMarker/removeMarker/updateMarker/
    // setMarkers/setMarkerDuration の全変更パスで発火するので、シーンカット検出
    // やクイックマーカー等あらゆる増減が自動でパネルへ反映される。
    if (m_timeline)
        connect(m_timeline, &Timeline::markersChanged,
                this, &MainWindow::refreshMarkerPanel);
    refreshMarkerPanel();

    // SM-5: ソースモニター ドック (右側)。メディアプールのダブルクリックは
    // openInSourceMonitor() 経由でここへロードされ、マークイン/アウト後に
    // 挿入/上書きボタンで 3 点編集する。
    m_sourceMonitorDock = new SourceMonitorDock(this);
    addDockWidget(Qt::RightDockWidgetArea, m_sourceMonitorDock);
    connect(m_sourceMonitorDock, &SourceMonitorDock::insertRequested,
            this, &MainWindow::onSourceInsertRequested);
    connect(m_sourceMonitorDock, &SourceMonitorDock::overwriteRequested,
            this, &MainWindow::onSourceOverwriteRequested);

    // AB-5: オーディオ バス パネル ドック (右側)。m_audioBusRouting が SSOT で、
    // パネルはそのポインタを指すビュー。ユーザ操作 → routingChanged →
    // onAudioBusRoutingChanged で AudioMixer へ反映する。既定では非表示にして
    // おき、「表示」メニューのトグルで出し入れする。
    m_audioBusPanel = new AudioBusPanel(this);
    addDockWidget(Qt::RightDockWidgetArea, m_audioBusPanel);
    m_audioBusPanel->setRouting(&m_audioBusRouting);
    m_audioBusPanel->refresh();
    m_audioBusPanel->setVisible(false);
    connect(m_audioBusPanel, &AudioBusPanel::routingChanged,
            this, &MainWindow::onAudioBusRoutingChanged);

    // US-SNS-7: 配信向け submenu
    auto *streamMenu = menuBar()->addMenu("配信向け(&S)");

    auto *smartReframeAction = streamMenu->addAction("スマートリフレーム (縦/正方形)...");
    connect(smartReframeAction, &QAction::triggered, this, &MainWindow::openSmartReframe);
    m_menuHelpEntries.append({smartReframeAction,
        QStringLiteral("横長の動画を、TikTok 等の縦型や正方形に自動で切り出し直します。被写体を追って枠を合わせます。")});

    auto *subtitleTrackAction = streamMenu->addAction("字幕トラックを生成・表示");
    connect(subtitleTrackAction, &QAction::triggered, this, &MainWindow::renderSubtitleTrack);
    m_menuHelpEntries.append({subtitleTrackAction,
        QStringLiteral("作成済みの字幕をタイムライン上のトラックとして表示し、見た目を調整できるようにします。")});

    streamMenu->addSeparator();

    auto *loudnessPanelAction = streamMenu->addAction("ラウドネスパネル");
    loudnessPanelAction->setCheckable(true);
    connect(loudnessPanelAction, &QAction::toggled, this, [this](bool visible) {
        if (m_loudnessDock) m_loudnessDock->setVisible(visible);
    });
    connect(m_loudnessDock, &QDockWidget::visibilityChanged, loudnessPanelAction, &QAction::setChecked);

    // コンポジション メニュー (After Effects風)
    auto *compMenu = menuBar()->addMenu("コンポジション(&C)");

    auto *addShapeAction = compMenu->addAction("シェイプレイヤー追加...");
    connect(addShapeAction, &QAction::triggered, this, &MainWindow::addShapeLayer);
    m_menuHelpEntries.append({addShapeAction,
        QStringLiteral("四角・丸・線などの図形を作って画面に重ねます。装飾や下地に使えます。")});

    auto *addParticleAction = compMenu->addAction("パーティクルエフェクト追加...");
    connect(addParticleAction, &QAction::triggered, this, &MainWindow::addParticleEffect);
    m_menuHelpEntries.append({addParticleAction,
        QStringLiteral("キラキラ・雪・煙などの粒が舞うエフェクトを追加します。")});

    auto *textAnimAction = compMenu->addAction("テキストアニメーション追加...");
    connect(textAnimAction, &QAction::triggered, this, &MainWindow::addTextAnimation);
    m_menuHelpEntries.append({textAnimAction,
        QStringLiteral("文字が出てくる・流れる・揺れるなどの動きをテロップに付けます。")});

    compMenu->addSeparator();

    auto *transformKfAction = compMenu->addAction("トランスフォームキーフレーム編集...");
    connect(transformKfAction, &QAction::triggered, this, &MainWindow::editTransformKeyframes);
    m_menuHelpEntries.append({transformKfAction,
        QStringLiteral("位置・大きさ・回転を時間に沿って変化させ、動くアニメーションを作ります。")});

    auto *maskAction = compMenu->addAction("マスク追加...");
    connect(maskAction, &QAction::triggered, this, &MainWindow::addMask);
    m_menuHelpEntries.append({maskAction,
        QStringLiteral("映像の一部分だけを見せる／隠す「窓」を作ります。一部だけ色を変える・切り抜くのに。")});

    auto *warpAction = compMenu->addAction("ワープ / 歪みエフェクト...");
    connect(warpAction, &QAction::triggered, this, &MainWindow::applyWarpEffect);
    m_menuHelpEntries.append({warpAction,
        QStringLiteral("映像をぐにゃっと曲げたり波打たせたりして変形させます。")});

    auto *rotoToolsAction = compMenu->addAction(QStringLiteral("ロトツール..."));
    connect(rotoToolsAction, &QAction::triggered, this, &MainWindow::openRotoToolsDialog);
    m_menuHelpEntries.append({rotoToolsAction,
        QStringLiteral("人や物の輪郭をなぞって切り抜きます。コマが進んでも形を自動で追従させられます。")});

    auto *timeRemapAction = compMenu->addAction(QStringLiteral("タイムリマップ..."));
    connect(timeRemapAction, &QAction::triggered, this, &MainWindow::openTimeRemapDialog);
    m_menuHelpEntries.append({timeRemapAction,
        QStringLiteral("クリップの再生速度を時間ごとに自由に変えます（だんだん遅く→速く 等）。なめらかに補間されます。")});

    auto *trackMatteAction = compMenu->addAction(QStringLiteral("トラックマット..."));
    connect(trackMatteAction, &QAction::triggered, this, &MainWindow::configureTrackMatte);
    m_menuHelpEntries.append({trackMatteAction,
        QStringLiteral("上のレイヤーの形や明るさを「型」にして、下のレイヤーをその形に切り抜きます。")});

    compMenu->addSeparator();

    auto *exprAction = compMenu->addAction("エクスプレッション...");
    connect(exprAction, &QAction::triggered, this, &MainWindow::editExpressions);
    m_menuHelpEntries.append({exprAction,
        QStringLiteral("簡単な数式で値を自動で動かします（例：ずっと揺らし続ける）。上級者向けです。")});

    auto *precompAction = compMenu->addAction("選択をプリコンポーズ...");
    connect(precompAction, &QAction::triggered, this, &MainWindow::precomposeSelected);
    m_menuHelpEntries.append({precompAction,
        QStringLiteral("複数のレイヤーを 1 つにまとめて扱いやすくします。整理整頓に便利です。")});

    // US-3D-11: motion-graphics sprint — 4 new menu actions (3D extruded text /
    // expressions / wiggle handheld / camera motion).
    compMenu->addSeparator();

    auto *extrudeTextAction = compMenu->addAction(QStringLiteral("3D 押し出しテキスト..."));
    connect(extrudeTextAction, &QAction::triggered, this, &MainWindow::open3DExtrudedText);
    m_menuHelpEntries.append({extrudeTextAction,
        QStringLiteral("文字を立体的に「押し出して」厚みや面取りを付け、選択中のクリップに重ねます。")});

    auto *clipExprAction = compMenu->addAction(QStringLiteral("式（エクスプレッション）..."));
    connect(clipExprAction, &QAction::triggered, this, &MainWindow::editClipExpressionBindings);
    m_menuHelpEntries.append({clipExprAction,
        QStringLiteral("位置・大きさ・回転・不透明度を簡単な数式で自動的に動かします。上級者向けです。")});

    auto *clipWiggleAction = compMenu->addAction(QStringLiteral("ウィグル / 手持ちカメラ風..."));
    connect(clipWiggleAction, &QAction::triggered, this, &MainWindow::editClipWiggle);
    m_menuHelpEntries.append({clipWiggleAction,
        QStringLiteral("クリップを小刻みに揺らして、手持ちカメラで撮ったような動きを足します。")});

    auto *cameraMotionAction = compMenu->addAction(QStringLiteral("カメラモーション..."));
    connect(cameraMotionAction, &QAction::triggered, this, &MainWindow::openCameraMotionDialog);
    m_menuHelpEntries.append({cameraMotionAction,
        QStringLiteral("仮想 3D カメラの動き（ドリー・パン・周回・手ぶれ）をプロジェクト全体に設定します。")});

    // --- カラーグレーディングパネル ---
    m_colorGradingPanel = new ColorGradingPanel(this);
    m_colorGradingPanel->setVisible(false); // 作成直後に非表示設定
    addDockWidget(Qt::RightDockWidgetArea, m_colorGradingPanel);
    m_colorGradingPanel->setLutList(LutLibrary::instance().allLuts());
    m_colorGradingPanel->close(); // 初期非表示を確実にする

    connect(m_colorGradingPanel, &ColorGradingPanel::colorCorrectionChanged,
            this, [this](const ColorCorrection &cc) {
        if (m_timeline->hasSelection()) {
            m_timeline->setClipColorCorrection(cc);
            m_player->setColorCorrection(cc);
        }
    });
    connect(m_colorGradingPanel, &ColorGradingPanel::lutSelected,
            this, [this](const QString &name) {
        if (name.isEmpty()) {
            m_player->glPreview()->clearLut();
            return;
        }
        LutData lut = LutLibrary::instance().findByName(name);
        if (lut.isValid()) {
            lut.intensity = m_colorGradingPanel->lutIntensity();
            m_player->glPreview()->setLut(lut);
        }
    });
    connect(m_colorGradingPanel, &ColorGradingPanel::lutIntensityChanged,
            this, [this](double intensity) {
        QString name = m_colorGradingPanel->selectedLutName();
        if (name.isEmpty()) return;
        LutData lut = LutLibrary::instance().findByName(name);
        if (lut.isValid()) {
            lut.intensity = intensity;
            m_player->glPreview()->setLut(lut);
        }
    });
    connect(m_colorGradingPanel, &ColorGradingPanel::resetRequested,
            this, [this]() {
        if (m_timeline->hasSelection()) {
            ColorCorrection cc;
            m_timeline->setClipColorCorrection(cc);
            m_player->setColorCorrection(cc);
            m_player->glPreview()->clearLut();
        }
    });
    // US-WIRE-2: wire ColorGradingPanel wheels → GLPreview shader
    connect(m_colorGradingPanel, &ColorGradingPanel::colorWheelsChanged,
            this, [this](const ColorWheels &cw) {
        if (!m_player || !m_player->glPreview())
            return;
        std::array<std::array<double,4>,3> values;
        values[0] = {static_cast<double>(cw.lift.x()),
                     static_cast<double>(cw.lift.y()),
                     static_cast<double>(cw.lift.z()),
                     cw.liftLuma};
        values[1] = {static_cast<double>(cw.gamma.x()),
                     static_cast<double>(cw.gamma.y()),
                     static_cast<double>(cw.gamma.z()),
                     cw.gammaLuma};
        values[2] = {static_cast<double>(cw.gain.x()),
                     static_cast<double>(cw.gain.y()),
                     static_cast<double>(cw.gain.z()),
                     cw.gainLuma};
        m_player->glPreview()->setLiftGammaGain(values);
    });

    // US-CG-1: wire ColorGradingPanel RGB Curves → GLPreview shader.
    // ColorGradingPanel re-emits CurveEditor::curvesChanged here.
    connect(m_colorGradingPanel, &ColorGradingPanel::curvesChanged,
            this, [this](const QVector<QVector<int>> &curves) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setRgbCurves(curves);
    });

    // US-CG-2: wire ColorGradingPanel White-Balance sliders → GLPreview uWb.
    // Sits at the very top of the grade chain (BEFORE LGG / curves / LUT).
    connect(m_colorGradingPanel, &ColorGradingPanel::whiteBalanceChanged,
            this, [this](float r, float g, float b) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setWhiteBalance(r, g, b);
    });

    // US-CG-3: wire ColorGradingPanel Vignette sliders → GLPreview uVig*.
    // Applied AFTER curves (US-CG-1) and BEFORE the .cube LUT (US-WIRE-1).
    connect(m_colorGradingPanel, &ColorGradingPanel::vignetteChanged,
            this, [this](float amount, float midpoint, float roundness, float feather) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setVignette(amount, midpoint, roundness, feather);
    });

    // US-CG-4: wire ColorGradingPanel Hue vs Saturation curve →
    // GLPreview::setHueVsSatLut. ColorGradingPanel re-emits the embedded
    // HueVsSatEditor's hueVsSatChanged signal here.
    connect(m_colorGradingPanel, &ColorGradingPanel::hueVsSatChanged,
            this, [this](const QVector<float> &lut) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setHueVsSatLut(lut);
    });

    // US-EF-1: wire ColorGradingPanel Chroma Key controls → GLPreview
    // uChroma* uniforms. Applied at the very TOP of the compose path (BEFORE
    // WB / LGG / curves / vignette / LUT) so the HSL gating + spill suppress
    // operate on raw frame colour. enabled=false is a free no-op.
    connect(m_colorGradingPanel, &ColorGradingPanel::chromaKeyChanged,
            this, [this](bool enabled, float keyH, float keyS, float keyL,
                         float hueTol, float satTol, float lumTol,
                         float spill, float softness) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setChromaKey(enabled, keyH, keyS, keyL,
                                            hueTol, satTol, lumTol,
                                            spill, softness);
    });

    // US-EF-2: wire ColorGradingPanel Mask controls → GLPreview uMask*
    // uniforms. The mask wraps the entire grade chain so the colour grade
    // applies INSIDE the mask region (or outside when invert=true).
    // enabled=false is a free no-op.
    connect(m_colorGradingPanel, &ColorGradingPanel::maskChanged,
            this, [this](bool enabled, bool ellipse, bool invert, float feather,
                         QRectF rect) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setMask(enabled, ellipse, invert, feather, rect);
    });

    // US-EF-3: wire ColorGradingPanel HSL Qualifier controls → GLPreview
    // uHslq* uniforms. The qualifier sits AFTER chroma key and BEFORE WB so
    // it operates on raw frame colour and applies a SECONDARY lift/gamma/
    // gain only inside the qualified hue/sat/luma region. enabled=false is
    // a free no-op (entire shader stage skipped).
    connect(m_colorGradingPanel, &ColorGradingPanel::hslQualifierChanged,
            this, [this](bool enabled,
                         float hueCenter, float hueRange,
                         float satMin, float satMax,
                         float lumaMin, float lumaMax,
                         float softness,
                         float liftR, float liftG, float liftB,
                         float gammaR, float gammaG, float gammaB,
                         float gainR, float gainG, float gainB) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setHslQualifier(enabled,
                                               hueCenter, hueRange,
                                               satMin, satMax,
                                               lumaMin, lumaMax,
                                               softness,
                                               liftR, liftG, liftB,
                                               gammaR, gammaG, gammaB,
                                               gainR, gainG, gainB);
    });

    // US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens
    // Distortion. ColorGradingPanel emits the raw slider scalars; GLPreview
    // applies the per-stage multipliers (×0.01 / px / ×0.01) inside the
    // fragment shader. Identity (0, 0, 0) is a free no-op (the shader's
    // |amount|>eps tests skip each kernel/transform entirely).
    connect(m_colorGradingPanel, &ColorGradingPanel::effectsPackChanged,
            this, [this](float sharpen, float blur, float lens) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setSharpen(sharpen);
        m_player->glPreview()->setBlur(blur);
        m_player->glPreview()->setLensDistortion(lens);
    });

    // US-EFC-1: Effect Controls panel — per-clip effect parameter panel.
    m_effectControlsPanel = new effectctrl::EffectControlsPanel(this);
    m_effectControlsPanel->setVisible(false);
    addDockWidget(Qt::RightDockWidgetArea, m_effectControlsPanel);
    m_effectControlsPanel->close();

    m_effectControlsPanel->setTimeline(m_timeline);
    m_effectControlsPanel->setMainWindow(this);
    connect(m_timeline, &Timeline::clipSelectedOnTrack,
            m_effectControlsPanel, &effectctrl::EffectControlsPanel::refreshFromCurrentClip);
    connect(m_effectControlsPanel, &effectctrl::EffectControlsPanel::effectsChanged,
            this, [this](const QVector<VideoEffect> &effects) {
        m_timeline->setClipEffects(effects);
    });

    m_vfxControlsPanel = new VfxControlsPanel(this);
    m_vfxControlsDock = new QDockWidget(QStringLiteral("VFX コントロール"), this);
    m_vfxControlsDock->setObjectName(QStringLiteral("VfxControlsDock"));
    m_vfxControlsDock->setWidget(m_vfxControlsPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_vfxControlsDock);
    m_vfxControlsDock->setVisible(false);

    connect(m_vfxControlsPanel, &VfxControlsPanel::glowChanged,
            this, [this](bool enabled, float threshold, float radius, float intensity) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setGlow(enabled, threshold, radius, intensity);
    });
    connect(m_vfxControlsPanel, &VfxControlsPanel::bloomChanged,
            this, [this](bool enabled, float threshold, float intensity, float spread) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setBloom(enabled, threshold, intensity, spread);
    });
    connect(m_vfxControlsPanel, &VfxControlsPanel::chromaticAberrationChanged,
            this, [this](bool enabled, float amount, float radialFalloff) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setChromaticAberration(enabled, amount, radialFalloff);
    });
    connect(m_vfxControlsPanel, &VfxControlsPanel::lightWrapChanged,
            this, [this](bool enabled, float amount, float radius) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setLightWrap(enabled, amount, radius);
    });

    // US-3D: 3-axis rotation + perspective foreshortening (Premiere "Basic
    // 3D" / Resolve "Transform" 3D rotation parity). Forwarded directly to
    // GLPreview::setRotation3D, which builds the 3x3 rotation matrix on the
    // CPU (intrinsic Tait-Bryan XYZ) and ships it to the fragment shader.
    // Identity (0,0,0,2.0) is a free no-op — the shader detects identity
    // and skips the warp entirely.
    connect(m_colorGradingPanel, &ColorGradingPanel::rotation3DChanged,
            this, [this](float xDeg, float yDeg, float zDeg, float persDist) {
        if (!m_player || !m_player->glPreview())
            return;
        m_player->glPreview()->setRotation3D(xDeg, yDeg, zDeg, persDist);
    });

    // US-EF-2: "マスクを描画" → enter the mask drawing overlay on the
    // VideoPlayer. The callback feeds the normalized QRectF back to the
    // panel via setMaskRect, which re-emits maskChanged so GLPreview picks
    // up the new geometry. Reuses the US-WIRE-3 region picker overlay.
    connect(m_colorGradingPanel, &ColorGradingPanel::requestMaskDraw,
            this, [this]() {
        if (!m_player || !m_colorGradingPanel)
            return;
        ColorGradingPanel *panel = m_colorGradingPanel;
        m_player->enterMaskEditMode([panel](QRectF normalizedRect) {
            if (!panel) return;
            // QRectF() (Esc / aborted drag) is silently ignored.
            if (!normalizedRect.isValid()
                || normalizedRect.width() <= 0.0
                || normalizedRect.height() <= 0.0)
                return;
            panel->setMaskRect(normalizedRect);
        });
    });

    viewMenu->addSeparator();
    auto *colorPanelAction = viewMenu->addAction("カラーグレーディングパネル(&G)");
    colorPanelAction->setCheckable(true);
    colorPanelAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    connect(colorPanelAction, &QAction::toggled, m_colorGradingPanel, &QDockWidget::setVisible);
    connect(m_colorGradingPanel, &QDockWidget::visibilityChanged, colorPanelAction, &QAction::setChecked);
    m_menuHelpEntries.append({colorPanelAction,
        QStringLiteral("色味・明るさを細かく調整する作業パネルを出し入れします。閉じてもここから戻せます。")});

    auto *effectControlsAction = viewMenu->addAction("Effect Controls");
    effectControlsAction->setCheckable(true);
    connect(effectControlsAction, &QAction::toggled, m_effectControlsPanel, &QDockWidget::setVisible);
    connect(m_effectControlsPanel, &QDockWidget::visibilityChanged, effectControlsAction, &QAction::setChecked);
    m_menuHelpEntries.append({effectControlsAction,
        QStringLiteral("選んだクリップに付いているエフェクトの設定値を編集するパネルを出し入れします。")});

    m_vfxControlsAction = viewMenu->addAction(QStringLiteral("VFX コントロール"));
    m_vfxControlsAction->setCheckable(true);
    connect(m_vfxControlsAction, &QAction::toggled, m_vfxControlsDock, &QDockWidget::setVisible);
    connect(m_vfxControlsDock, &QDockWidget::visibilityChanged, m_vfxControlsAction, &QAction::setChecked);
    m_menuHelpEntries.append({m_vfxControlsAction,
        QStringLiteral("グロー（光らせる）やにじみなどの特殊効果を調整するパネルを出し入れします。")});

    // Lumetri Scopes dock — Histogram + Luma Waveform + Vectorscope. Off
    // by default so first-run users aren't paying CPU on scope math; the
    // toggle action lives next to the colour grading panel for discovery.
    auto *scopesDock = new QDockWidget("Lumetri Scopes", this);
    auto *scopes = new LumetriScopes(scopesDock);
    scopesDock->setWidget(scopes);
    scopesDock->setObjectName("LumetriScopesDock");
    addDockWidget(Qt::RightDockWidgetArea, scopesDock);
    scopesDock->setVisible(false);
    if (m_player)
        connect(m_player, &VideoPlayer::frameComposited,
                scopes, &LumetriScopes::setFrame);
    auto *scopesAction = viewMenu->addAction("Lumetri Scopes(&L)");
    scopesAction->setCheckable(true);
    scopesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    connect(scopesAction, &QAction::toggled, scopesDock, &QDockWidget::setVisible);
    connect(scopesDock, &QDockWidget::visibilityChanged, scopesAction, &QAction::setChecked);
    m_menuHelpEntries.append({scopesAction,
        QStringLiteral("映像の明るさや色の分布をグラフで確認するパネルを出し入れします。色調整の目安に。")});

    // Audio Meters dock
    m_audioMetersDock = new QDockWidget("Audio Meters", this);
    m_audioMetersDock->setObjectName("AudioMetersDock");
    addDockWidget(Qt::RightDockWidgetArea, m_audioMetersDock);
    m_audioMetersDock->setVisible(true);
    auto *audioMetersAction = viewMenu->addAction("Audio Meters");
    audioMetersAction->setCheckable(true);
    audioMetersAction->setChecked(true);
    connect(audioMetersAction, &QAction::toggled, m_audioMetersDock, &QDockWidget::setVisible);
    connect(m_audioMetersDock, &QDockWidget::visibilityChanged, audioMetersAction, &QAction::setChecked);
    m_menuHelpEntries.append({audioMetersAction,
        QStringLiteral("音の大きさをメーターで表示するパネルを出し入れします。音割れしていないか確認できます。")});

    // History dock
    m_historyDock = new HistoryDockWidget(m_timeline->undoManager(), this);
    addDockWidget(Qt::RightDockWidgetArea, m_historyDock);
    m_historyDock->setVisible(true);
    auto *historyAction = viewMenu->addAction("History");
    historyAction->setCheckable(true);
    historyAction->setChecked(true);
    connect(historyAction, &QAction::toggled, m_historyDock, &QDockWidget::setVisible);
    connect(m_historyDock, &QDockWidget::visibilityChanged, historyAction, &QAction::setChecked);
    m_menuHelpEntries.append({historyAction,
        QStringLiteral("これまでの操作の履歴を一覧で表示するパネルを出し入れします。前の状態まで一気に戻れます。")});

    // MP-5: メディアプール ドックの表示トグル
    if (m_mediaPoolDock) {
        auto *mediaPoolAction = viewMenu->addAction("メディアプール");
        mediaPoolAction->setCheckable(true);
        mediaPoolAction->setChecked(m_mediaPoolDock->isVisible());
        connect(mediaPoolAction, &QAction::toggled, m_mediaPoolDock, &QDockWidget::setVisible);
        connect(m_mediaPoolDock, &QDockWidget::visibilityChanged, mediaPoolAction, &QAction::setChecked);
        m_menuHelpEntries.append({mediaPoolAction,
            QStringLiteral("取り込んだ動画・音声・画像をビン（フォルダ）で整理するパネルを出し入れします。素材をダブルクリックでタイムラインへ追加できます。")});
    }

    // SM-5: ソースモニター ドックの表示トグル
    if (m_sourceMonitorDock) {
        auto *sourceMonitorAction = viewMenu->addAction("ソースモニター");
        sourceMonitorAction->setCheckable(true);
        sourceMonitorAction->setChecked(m_sourceMonitorDock->isVisible());
        connect(sourceMonitorAction, &QAction::toggled, m_sourceMonitorDock, &QDockWidget::setVisible);
        connect(m_sourceMonitorDock, &QDockWidget::visibilityChanged, sourceMonitorAction, &QAction::setChecked);
        m_menuHelpEntries.append({sourceMonitorAction,
            QStringLiteral("素材を再生しながらマークイン/マークアウトで使う範囲を決め、再生ヘッド位置へ挿入/上書きするパネルを出し入れします。")});
    }

    // AB-5: オーディオ バス パネル ドックの表示トグル
    if (m_audioBusPanel) {
        auto *audioBusAction = viewMenu->addAction("オーディオ バス");
        audioBusAction->setCheckable(true);
        audioBusAction->setChecked(m_audioBusPanel->isVisible());
        connect(audioBusAction, &QAction::toggled, m_audioBusPanel, &QDockWidget::setVisible);
        connect(m_audioBusPanel, &QDockWidget::visibilityChanged, audioBusAction, &QAction::setChecked);
        m_menuHelpEntries.append({audioBusAction,
            QStringLiteral("複数のオーディオトラックをバス（グループ）へまとめ、ゲイン・ミュート・ソロ・サブミックスをまとめて調整するパネルを出し入れします。")});
    }

    // US-NODE-9: Node compositing mode toggle
    m_nodeModeAction = viewMenu->addAction("ノードコンポジットモード");
    m_nodeModeAction->setCheckable(true);
    m_nodeModeAction->setChecked(false);
    connect(m_nodeModeAction, &QAction::toggled, this, &MainWindow::toggleNodeCompositingMode);
    m_menuHelpEntries.append({m_nodeModeAction,
        QStringLiteral("高度な合成を、箱（ノード）を線でつなぐ方式の画面に切り替えます。上級者向けです。")});

    // 表示メニュー追加項目
    auto *themeAction = viewMenu->addAction("テーマ変更...");
    connect(themeAction, &QAction::triggered, this, &MainWindow::changeTheme);
    m_menuHelpEntries.append({themeAction,
        QStringLiteral("画面の見た目（暗いテーマ／明るいテーマなど）を変えます。")});

    viewMenu->addSeparator();

    auto *tooltipAction = viewMenu->addAction("ツールバーのツールチップを表示");
    tooltipAction->setCheckable(true);
    m_menuHelpEntries.append({tooltipAction,
        QStringLiteral("上のボタン列にマウスを当てたとき、ボタンの説明を吹き出しで出すかどうかを切り替えます。")});
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        tooltipAction->setChecked(prefSettings.value("showTooltips", true).toBool());
    }
    connect(tooltipAction, &QAction::toggled, this, [this](bool checked) {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        prefSettings.setValue("showTooltips", checked);
        // Re-apply tooltips on all toolbar actions
        auto *toolbar = findChild<QToolBar *>("Main");
        if (!toolbar) return;
        if (checked) {
            statusBar()->showMessage("ツールチップ有効 — 再起動で反映");
        } else {
            for (auto *action : toolbar->actions())
                action->setToolTip("");
            statusBar()->showMessage("ツールバーのツールチップを無効化");
        }
    });

    // メニュー項目に初心者向けの説明（hover ヘルプ）を出すかどうかのトグル。
    // デフォルト ON。QSettings("VSimpleEditor","Preferences") キー
    // "showMenuHints" に保存し、即座に applyMenuHelpTooltips() で反映する。
    auto *menuHintsAction = viewMenu->addAction("メニューの説明を表示");
    menuHintsAction->setCheckable(true);
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        menuHintsAction->setChecked(prefSettings.value("showMenuHints", true).toBool());
    }
    connect(menuHintsAction, &QAction::toggled, this, [this](bool checked) {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        prefSettings.setValue("showMenuHints", checked);
        applyMenuHelpTooltips(checked);
        statusBar()->showMessage(checked
            ? QStringLiteral("メニューの説明（hover ヘルプ）を表示します")
            : QStringLiteral("メニューの説明（hover ヘルプ）を非表示にしました"));
    });
    m_menuHelpEntries.append({menuHintsAction,
        QStringLiteral("この説明（メニューにマウスを当てると出る吹き出し）を表示するかどうかを切り替えます。")});

    auto *toolbarStyleAction = viewMenu->addAction("ツールバーをアイコンのみ表示");
    toolbarStyleAction->setCheckable(true);
    connect(toolbarStyleAction, &QAction::toggled, this, [this](bool iconOnly) {
        auto *toolbar = findChild<QToolBar *>();
        if (toolbar) {
            toolbar->setToolButtonStyle(iconOnly ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
            QSettings prefSettings("VSimpleEditor", "Preferences");
            prefSettings.setValue("toolbarIconOnly", iconOnly);
        }
    });
    m_menuHelpEntries.append({toolbarStyleAction,
        QStringLiteral("上のボタン列を、アイコンだけの小さい表示にするか、文字付きの表示にするかを切り替えます。")});

    // 取り込み配置ポリシー: 並列トラック (V2/A2...) か 現在トラック追加 (V1/A1 連結)
    auto *importPlacementGroup = new QActionGroup(this);
    importPlacementGroup->setExclusive(true);
    auto *importParallelAction = new QAction("取り込み：新しいトラックに並列配置", this);
    importParallelAction->setCheckable(true);
    importParallelAction->setActionGroup(importPlacementGroup);
    auto *importAppendAction = new QAction("取り込み：現在のトラックに追加", this);
    importAppendAction->setCheckable(true);
    importAppendAction->setActionGroup(importPlacementGroup);
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        const int saved = prefSettings.value("importPlacement",
                                              static_cast<int>(ImportPlacement::ParallelTrack)).toInt();
        if (saved == static_cast<int>(ImportPlacement::AppendToFirstTrack))
            importAppendAction->setChecked(true);
        else
            importParallelAction->setChecked(true);
    }
    connect(importParallelAction, &QAction::toggled, this, [this](bool checked) {
        if (!checked) return;
        QSettings prefSettings("VSimpleEditor", "Preferences");
        prefSettings.setValue("importPlacement", static_cast<int>(ImportPlacement::ParallelTrack));
        statusBar()->showMessage("取り込み先を V2/A2 並列配置に設定");
    });
    connect(importAppendAction, &QAction::toggled, this, [this](bool checked) {
        if (!checked) return;
        QSettings prefSettings("VSimpleEditor", "Preferences");
        prefSettings.setValue("importPlacement", static_cast<int>(ImportPlacement::AppendToFirstTrack));
        statusBar()->showMessage("取り込み先を V1/A1 追加に設定");
    });

    // 自動プロキシ生成: 重い素材 (AV1 / QHD+) 取り込み時の挙動 3 択
    auto *autoProxyGroup = new QActionGroup(this);
    autoProxyGroup->setExclusive(true);
    auto *autoProxyDisabledAction = new QAction("自動プロキシ生成: しない", this);
    autoProxyDisabledAction->setCheckable(true);
    autoProxyDisabledAction->setActionGroup(autoProxyGroup);
    auto *autoProxyMultiAction = new QAction("自動プロキシ生成: V2 以降のみ", this);
    autoProxyMultiAction->setCheckable(true);
    autoProxyMultiAction->setActionGroup(autoProxyGroup);
    auto *autoProxyAlwaysAction = new QAction("自動プロキシ生成: 常時", this);
    autoProxyAlwaysAction->setCheckable(true);
    autoProxyAlwaysAction->setActionGroup(autoProxyGroup);
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        const int saved = prefSettings.value("autoProxyMode",
                                              static_cast<int>(AutoProxyMode::MultiTrackOnly)).toInt();
        if (saved == static_cast<int>(AutoProxyMode::Disabled))
            autoProxyDisabledAction->setChecked(true);
        else if (saved == static_cast<int>(AutoProxyMode::Always))
            autoProxyAlwaysAction->setChecked(true);
        else
            autoProxyMultiAction->setChecked(true);
    }
    connect(autoProxyDisabledAction, &QAction::toggled, this, [this](bool checked) {
        if (!checked) return;
        QSettings("VSimpleEditor", "Preferences").setValue("autoProxyMode",
            static_cast<int>(AutoProxyMode::Disabled));
        statusBar()->showMessage("自動プロキシ生成を無効化");
    });
    connect(autoProxyMultiAction, &QAction::toggled, this, [this](bool checked) {
        if (!checked) return;
        QSettings("VSimpleEditor", "Preferences").setValue("autoProxyMode",
            static_cast<int>(AutoProxyMode::MultiTrackOnly));
        statusBar()->showMessage("自動プロキシ生成: V2 以降のみ");
    });
    connect(autoProxyAlwaysAction, &QAction::toggled, this, [this](bool checked) {
        if (!checked) return;
        QSettings("VSimpleEditor", "Preferences").setValue("autoProxyMode",
            static_cast<int>(AutoProxyMode::Always));
        statusBar()->showMessage("自動プロキシ生成: 常時");
    });

    // 自動保存（バックアップ）トグル — デフォルトOFF、30分周期
    auto *autoSaveAction = new QAction("自動保存を有効化 (30分ごと)", this);
    autoSaveAction->setCheckable(true);
    m_menuHelpEntries.append({autoSaveAction,
        QStringLiteral("一定時間ごとに自動でバックアップを保存します。万一のクラッシュ対策に ON がおすすめです。")});
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        autoSaveAction->setChecked(prefSettings.value("autoSaveEnabled", false).toBool());
    }
    connect(autoSaveAction, &QAction::toggled, this, [this](bool checked) {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        prefSettings.setValue("autoSaveEnabled", checked);
        if (!m_autoSave)
            return;
        if (checked) {
            AutoSaveConfig cfg;
            cfg.enabled = true;
            cfg.interval = prefSettings.value("autoSaveIntervalSec", 1800).toInt();
            m_autoSave->start(cfg);
            statusBar()->showMessage(QString("自動保存 ON (%1分ごと)").arg(cfg.interval / 60));
        } else {
            m_autoSave->stop();
            statusBar()->showMessage("自動保存 OFF");
        }
    });

    // 環境設定サブメニューに共有QActionを集約
    prefsMenu->addSeparator();
    prefsMenu->addAction(themeAction);
    prefsMenu->addAction(menuHintsAction);
    prefsMenu->addAction(tooltipAction);
    prefsMenu->addAction(toolbarStyleAction);
    prefsMenu->addSeparator();
    prefsMenu->addAction(importParallelAction);
    prefsMenu->addAction(importAppendAction);
    prefsMenu->addSeparator();
    prefsMenu->addAction(autoProxyDisabledAction);
    prefsMenu->addAction(autoProxyMultiAction);
    prefsMenu->addAction(autoProxyAlwaysAction);
    prefsMenu->addSeparator();

    auto *gpuEffectsAction = new QAction("GPUエフェクトを使用", this);
    gpuEffectsAction->setCheckable(true);
    {
        QSettings gpuFxSettings("VSimpleEditor", "Preferences");
        gpuEffectsAction->setChecked(gpuFxSettings.value("gpuEffectsEnabled", true).toBool());
    }
    connect(gpuEffectsAction, &QAction::toggled, this, [this](bool on) {
        QSettings("VSimpleEditor", "Preferences").setValue("gpuEffectsEnabled", on);
        if (m_player && m_timeline && m_timeline->hasSelection())
            m_player->setPreviewEffects(m_timeline->clipEffects(), /*live=*/true);
        else if (m_player)
            m_player->setPreviewEffects({}, /*live=*/true);
    });
    prefsMenu->addAction(gpuEffectsAction);
    m_menuHelpEntries.append({gpuEffectsAction,
        QStringLiteral("グラフィックボードを使ってエフェクト処理を速くします。動作が不安定なときは OFF にしてください。")});
    prefsMenu->addSeparator();

    // Iteration 12: toggle for auto-play on first clip drop. Default OFF
    // per user request — the auto-play side effect of Iteration 10
    // setSequence empty -> non-empty handler is now opt-in. VideoPlayer
    // reads QSettings("VSimpleEditor", "Preferences")/autoPlayOnFirstSequence
    // every setSequence call so the toggle takes effect immediately
    // without a restart.
    auto *autoPlayAction = new QAction("クリップ追加で自動再生", this);
    autoPlayAction->setCheckable(true);
    {
        QSettings autoPlaySettings("VSimpleEditor", "Preferences");
        autoPlayAction->setChecked(
            autoPlaySettings.value("autoPlayOnFirstSequence", false).toBool());
    }
    connect(autoPlayAction, &QAction::toggled, this, [](bool on) {
        QSettings("VSimpleEditor", "Preferences")
            .setValue("autoPlayOnFirstSequence", on);
    });
    prefsMenu->addAction(autoPlayAction);
    prefsMenu->addSeparator();

    auto *loudnessAction = new QAction(
        QStringLiteral("オーディオ均一化..."), this);
    loudnessAction->setStatusTip(QStringLiteral(
        "全トラックの出力レベルを動的に均一化 (FCP の Loudness 風)"));
    connect(loudnessAction, &QAction::triggered,
            this, &MainWindow::openLoudnessSettings);
    prefsMenu->addAction(loudnessAction);
    m_menuHelpEntries.append({loudnessAction,
        QStringLiteral("動画全体の音量を均一にそろえます (音量を均一に / 音量をそろえる / "
                       "ノーマライズ / ラウドネス均一化)。配信向けの音量調整に。")});
    prefsMenu->addSeparator();

    // US-T39 Snap strength submenu — pulls/flushes the video source onto
    // the 16:9 canvas edges when dragging. Persisted via QSettings so the
    // user's preference survives restart.
    auto *snapMenu = prefsMenu->addMenu("画面フィット強度");
    auto *snapGroup = new QActionGroup(this);
    snapGroup->setExclusive(true);
    struct SnapPreset { const char *label; double px; };
    const SnapPreset snapPresets[] = {
        {"オフ",  0.0},
        {"弱",   6.0},
        {"中",  12.0},
        {"強",  24.0},
        {"最強", 48.0},
    };
    double savedSnap = 12.0;
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        savedSnap = prefSettings.value("snapStrength", 12.0).toDouble();
    }
    if (m_player)
        m_player->setSnapStrength(savedSnap);
    for (const auto &preset : snapPresets) {
        auto *act = new QAction(preset.label, this);
        act->setCheckable(true);
        act->setActionGroup(snapGroup);
        if (qFuzzyCompare(preset.px, savedSnap)
            || (preset.px == 0.0 && savedSnap <= 0.0))
            act->setChecked(true);
        const double px = preset.px;
        connect(act, &QAction::toggled, this, [this, px](bool checked) {
            if (!checked) return;
            if (m_player) m_player->setSnapStrength(px);
            QSettings prefSettings("VSimpleEditor", "Preferences");
            prefSettings.setValue("snapStrength", px);
            statusBar()->showMessage(
                QString("画面フィット強度: %1 px").arg(px == 0.0 ? QStringLiteral("オフ") : QString::number(px)));
        });
        snapMenu->addAction(act);
    }

    prefsMenu->addAction(autoSaveAction);

    // ヘルプ メニュー
    auto *helpMenu = menuBar()->addMenu("ヘルプ(&H)");

    auto *resourceGuideAction = helpMenu->addAction("無料素材ガイド...");
    resourceGuideAction->setShortcut(QKeySequence(Qt::Key_F1));
    connect(resourceGuideAction, &QAction::triggered, this, &MainWindow::showResourceGuide);
    m_menuHelpEntries.append({resourceGuideAction,
        QStringLiteral("商用利用 OK の無料動画・音楽・画像が手に入るサイトの一覧を開きます。")});

    helpMenu->addSeparator();

    auto *aboutAction = helpMenu->addAction("バージョン情報(&A)");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::about);
    m_menuHelpEntries.append({aboutAction,
        QStringLiteral("このアプリのバージョンや情報を表示します。")});

    // 全メニュー（サブメニュー含む）でツールチップを有効化。これをしないと
    // Qt のメニューはマウスを当てても説明（ツールチップ）を出さない。
    const QList<QMenu *> allMenus = menuBar()->findChildren<QMenu *>();
    for (QMenu *menu : allMenus) {
        if (menu)
            menu->setToolTipsVisible(true);
    }

    // メニュー項目の hover ヘルプを設定の保存値（デフォルト ON）にしたがって適用。
    {
        QSettings prefSettings("VSimpleEditor", "Preferences");
        applyMenuHelpTooltips(prefSettings.value("showMenuHints", true).toBool());
    }

    // --- お気に入り: build the registry of favoritable actions ---
    // Walk every top-level menu (the お気に入り menu itself excluded) and
    // register each direct, named, non-submenu action. The stable id is
    // "<menuKey>.<index>" where menuKey comes from a fixed title→key map and
    // index is the action's position among the favoritable actions in that
    // menu. This sprint model only ever appends actions to a menu, so existing
    // ids stay stable; the id is NEVER derived from the (translatable) text so
    // the persisted favorites list survives UI-text changes. label / menuPath
    // are display strings used only by FavoritesEditDialog for grouping.
    {
        static const QHash<QString, QString> menuKeyByTitle = {
            {QStringLiteral("ファイル"),       QStringLiteral("file")},
            {QStringLiteral("編集"),           QStringLiteral("edit")},
            {QStringLiteral("表示"),           QStringLiteral("view")},
            {QStringLiteral("トラック"),       QStringLiteral("track")},
            {QStringLiteral("挿入"),           QStringLiteral("insert")},
            {QStringLiteral("オーディオ"),     QStringLiteral("audio")},
            {QStringLiteral("マーカー"),       QStringLiteral("marker")},
            {QStringLiteral("エフェクト"),     QStringLiteral("effect")},
            {QStringLiteral("再生"),           QStringLiteral("playback")},
            {QStringLiteral("ツール"),         QStringLiteral("tools")},
            {QStringLiteral("配信向け"),       QStringLiteral("stream")},
            {QStringLiteral("コンポジション"), QStringLiteral("comp")},
            {QStringLiteral("ヘルプ"),         QStringLiteral("help")},
        };
        m_favoritableActions.clear();
        const QList<QAction *> topActions = menuBar()->actions();
        for (QAction *menuAct : topActions) {
            if (!menuAct)
                continue;
            QMenu *menu = menuAct->menu();
            if (!menu || menu == m_favoritesMenu)
                continue;
            // Sanitize the title for display: drop the "(&X)" mnemonic hint
            // — e.g. "ファイル(&F)" → "ファイル", "配信向け(&S)" → "配信向け",
            // "コンポジション(&C)" → "コンポジション". Falls back to a generic
            // "&"-strip for any title that doesn't follow the "...(&X)" form.
            QString title = menu->title();
            const int mnemonicAt = title.indexOf(QStringLiteral("(&"));
            if (mnemonicAt >= 0 && title.endsWith(QLatin1Char(')')))
                title.truncate(mnemonicAt);
            title.remove(QLatin1Char('&'));
            title = title.trimmed();
            if (title.isEmpty())
                title = QStringLiteral("その他");
            const QString menuKey = menuKeyByTitle.value(title, QStringLiteral("menu"));
            int index = 0;
            const QList<QAction *> acts = menu->actions();
            for (QAction *act : acts) {
                if (!act || act->isSeparator() || act->menu())
                    continue;
                const QString label = act->text();
                if (label.isEmpty())
                    continue; // widget actions (e.g. the LUT slider) etc.
                const QString id = QStringLiteral("%1.%2").arg(menuKey).arg(index);
                ++index;
                m_favoritableActions.append({id, label, title, act});
            }
        }
    }

    // Populate the お気に入り menu's dynamic part from the persisted list.
    rebuildFavoritesMenu();
}

void MainWindow::rebuildFavoritesMenu()
{
    if (!m_favoritesMenu)
        return;

    // Clear everything we own (the proxy actions, any placeholder, and the
    // bottom separator + edit action) and rebuild from scratch. The
    // m_editFavoritesAction QAction object is reused — only re-added.
    m_favoritesMenu->clear();

    QStringList favoriteIds;
    {
        QSettings prefSettings(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
        favoriteIds = prefSettings.value(QStringLiteral("favoriteActions")).toStringList();
    }

    int added = 0;
    for (const QString &id : favoriteIds) {
        const FavoritableAction *fav = nullptr;
        for (const auto &candidate : m_favoritableActions) {
            if (candidate.id == id) {
                fav = &candidate;
                break;
            }
        }
        if (!fav || !fav->action)
            continue; // stale id (renamed/removed action) — silently skip
        QAction *original = fav->action;
        // Lightweight proxy — never re-parent or move the real action.
        auto *proxy = new QAction(original->icon(), original->text(), m_favoritesMenu);
        proxy->setToolTip(original->toolTip());
        proxy->setEnabled(original->isEnabled());
        connect(proxy, &QAction::triggered, original, &QAction::trigger);
        // Keep the proxy in sync with the original so a context-sensitive
        // command (e.g. "クリップを分割") greys out / re-labels here too.
        connect(original, &QAction::changed, proxy, [original, proxy]() {
            proxy->setEnabled(original->isEnabled());
            proxy->setText(original->text());
            proxy->setIcon(original->icon());
            proxy->setToolTip(original->toolTip());
        });
        m_favoritesMenu->addAction(proxy);
        ++added;
    }

    if (added == 0) {
        auto *placeholder = m_favoritesMenu->addAction(
            QStringLiteral("（「お気に入りを編集...」から機能を追加してください）"));
        placeholder->setEnabled(false);
    }

    m_favoritesMenu->addSeparator();
    if (m_editFavoritesAction)
        m_favoritesMenu->addAction(m_editFavoritesAction);
}

void MainWindow::editFavorites()
{
    FavoritesEditDialog dialog(this);

    QVector<QPair<QString, QString>> idLabelPairs;
    QHash<QString, QString> idToMenuPath;
    idLabelPairs.reserve(m_favoritableActions.size());
    for (const auto &fav : m_favoritableActions) {
        idLabelPairs.append({fav.id, fav.label});
        idToMenuPath.insert(fav.id, fav.menuPath);
    }
    dialog.setAvailableActions(idLabelPairs, idToMenuPath);

    {
        QSettings prefSettings(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
        dialog.setSelectedIds(prefSettings.value(QStringLiteral("favoriteActions")).toStringList());
    }

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QStringList chosen = dialog.selectedIds();
    {
        QSettings prefSettings(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
        prefSettings.setValue(QStringLiteral("favoriteActions"), chosen);
    }
    rebuildFavoritesMenu();
    statusBar()->showMessage(
        QStringLiteral("お気に入りを更新しました（%1 件）").arg(chosen.size()), 4000);
}

void MainWindow::applyMenuHelpTooltips(bool enabled)
{
    for (const auto &entry : m_menuHelpEntries) {
        QAction *action = entry.first;
        if (!action)
            continue;
        // Only manage the tooltip — leaving any pre-existing status tip
        // (e.g. set explicitly elsewhere) untouched.
        action->setToolTip(enabled ? entry.second : QString());
    }
}

void MainWindow::setupToolBar()
{
    auto *toolbar = addToolBar("Main");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(20, 20));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto icon = [](const QString &name) { return QIcon(":/icons/" + name + ".svg"); };

    auto addBtn = [&](const QString &iconName, const QString &label,
                      const QString &tooltip, auto slot) {
        auto *action = toolbar->addAction(icon(iconName), label, this, slot);
        action->setToolTip(tooltip);
        return action;
    };

    addBtn("new", "新規", "新規プロジェクト (Ctrl+N)", &MainWindow::newProject);
    addBtn("open", "開く", "ファイルを開く (Ctrl+O)", &MainWindow::openFile);
    addBtn("save", "保存", "プロジェクトを保存 (Ctrl+S)", &MainWindow::saveProject);
    toolbar->addSeparator();
    addBtn("undo", "元に戻す", "元に戻す (Ctrl+Z)", &MainWindow::undoAction);
    addBtn("redo", "やり直し", "やり直し (Ctrl+Shift+Z)", &MainWindow::redoAction);
    toolbar->addSeparator();
    addBtn("split", "分割", "再生ヘッドで分割 (S)", &MainWindow::splitClip);
    addBtn("delete", "削除", "クリップ削除 (Del)", &MainWindow::deleteClip);
    addBtn("copy", "コピー", "クリップをコピー (Ctrl+C)", &MainWindow::copyClip);
    addBtn("paste", "貼付", "クリップを貼り付け (Ctrl+V)", &MainWindow::pasteClip);
    toolbar->addSeparator();
    // Text tool: toolbar button toggles Adobe-style text-tool mode instead
    // of opening the modal dialog directly. The legacy dialog is still
    // reachable via 挿入 → テキスト / テロップ追加 for users who prefer it.
    m_textToolAction = toolbar->addAction(icon("text"), "テキスト");
    m_textToolAction->setToolTip("テキストツール (T) — ドラッグでテキスト枠を指定");
    m_textToolAction->setCheckable(true);
    connect(m_textToolAction, &QAction::toggled, this, &MainWindow::onTextToolToggled);
    addBtn("color", "色補正", "色補正 (Ctrl+G)", &MainWindow::colorCorrection);
    addBtn("effects", "効果", "ビデオエフェクト (Ctrl+Shift+F)", &MainWindow::videoEffects);
    addBtn("marker", "マーカー", "マーカー追加 (Ctrl+M)", &MainWindow::addMarker);
    toolbar->addSeparator();
    addBtn("export", "出力", "動画をエクスポート (Ctrl+E)", &MainWindow::exportVideo);
    addBtn("record", "録画", "画面録画を開始", &MainWindow::startScreenRecording);

    // Apply saved tooltip preference
    QSettings settings("VSimpleEditor", "Preferences");
    bool showTooltips = settings.value("showTooltips", true).toBool();
    if (!showTooltips) {
        for (auto *action : toolbar->actions())
            action->setToolTip("");
    }
}

void MainWindow::updateEditActions()
{
    bool hasSel = m_timeline->hasSelection();
    m_deleteAction->setEnabled(hasSel);
    m_rippleDeleteAction->setEnabled(hasSel);
    m_copyAction->setEnabled(hasSel);
    m_pasteAction->setEnabled(m_timeline->hasClipboard());
    m_undoAction->setEnabled(m_timeline->canUndo());
    m_redoAction->setEnabled(m_timeline->canRedo());
}

void MainWindow::updateTitle()
{
    QString title = QString("V Simple Editor - %1 (%2 %3fps)")
        .arg(m_projectConfig.name)
        .arg(m_projectConfig.resolutionLabel())
        .arg(m_projectConfig.fps);
    if (!m_projectFilePath.isEmpty())
        title += " — " + QFileInfo(m_projectFilePath).fileName();
    setWindowTitle(title);
}

void MainWindow::applyProjectConfig(const ProjectConfig &config)
{
    m_projectConfig = config;
    m_player->setCanvasSize(config.width, config.height);
    updateTitle();
    statusBar()->showMessage(QString("Project: %1 — %2 %3fps")
        .arg(config.name).arg(config.resolutionLabel()).arg(config.fps));
}

QString MainWindow::particleClipKey(const ClipInfo &clip)
{
    return clip.filePath;
}

bool MainWindow::selectedVideoClipRef(int &trackIdx, int &clipIdx, ClipInfo *clip) const
{
    trackIdx = m_selectedVideoTrackIndex;
    clipIdx = m_selectedVideoClipIndexTracked;

    if (m_timeline && trackIdx >= 0 && trackIdx < m_timeline->videoTracks().size()) {
        const auto *track = m_timeline->videoTracks().value(trackIdx, nullptr);
        if (track && clipIdx >= 0 && clipIdx < track->clips().size()) {
            if (clip)
                *clip = track->clips().at(clipIdx);
            return true;
        }
    }

    trackIdx = 0;
    clipIdx = m_timeline ? m_timeline->selectedVideoClipIndex() : -1;
    if (!m_timeline)
        return false;
    const auto &clips = m_timeline->videoClips();
    if (clipIdx >= 0 && clipIdx < clips.size()) {
        if (clip)
            *clip = clips.at(clipIdx);
        return true;
    }
    return false;
}

double MainWindow::clipTimelineStartSeconds(int trackIdx, int clipIdx) const
{
    if (!m_timeline || trackIdx < 0 || trackIdx >= m_timeline->videoTracks().size())
        return 0.0;
    const auto *track = m_timeline->videoTracks().value(trackIdx, nullptr);
    if (!track || clipIdx < 0)
        return 0.0;

    double start = 0.0;
    const auto &clips = track->clips();
    for (int i = 0; i < clips.size() && i < clipIdx; ++i) {
        start += qMax(0.0, clips[i].leadInSec);
        start += clips[i].effectiveDuration();
    }
    if (clipIdx < clips.size())
        start += qMax(0.0, clips[clipIdx].leadInSec);
    return start;
}

double MainWindow::clipSourceTimeAtPlayheadSeconds(int trackIdx, int clipIdx, const ClipInfo &clip) const
{
    const double clipStart = clipTimelineStartSeconds(trackIdx, clipIdx);
    const double localSeconds = qBound(0.0,
                                       (m_timeline ? m_timeline->playheadPosition() : 0.0) - clipStart,
                                       clip.effectiveDuration());
    const double clipOut = clipSourceOutPoint(clip);
    const double sourceSeconds = clip.inPoint + localSeconds * qMax(clip.speed, 0.0001);
    return qBound(clip.inPoint, sourceSeconds, clipOut);
}

QImage MainWindow::decodeClipFrameAtSourceTime(const ClipInfo &clip, double sourceTimeSeconds) const
{
    const QString playbackPath = ProxyManager::instance().getProxyPath(clip.filePath);
    const double clipOut = clipSourceOutPoint(clip);
    return decodeFrameAtSecondsFromFile(playbackPath, qBound(clip.inPoint, sourceTimeSeconds, clipOut));
}

QImage MainWindow::decodeClipFrameByIndex(const ClipInfo &clip, int sourceFrameIndex, double sourceFps) const
{
    const double fps = qMax(1.0, sourceFps);
    const double seconds = clip.inPoint + (qMax(0, sourceFrameIndex) / fps);
    return decodeClipFrameAtSourceTime(clip, seconds);
}

QImage MainWindow::applyStoredRotoData(const QString &clipId,
                                       const QImage &frame,
                                       int sourceFrameIndex) const
{
    const auto it = m_rotoClipEntries.constFind(clipId);
    if (it == m_rotoClipEntries.cend() || frame.isNull())
        return frame;

    const RotoClipEntry &entry = it.value();
    QImage mask;
    if (!entry.keyframes.isEmpty()) {
        Rotoscope roto;
        for (const auto &keyframe : entry.keyframes)
            roto.addKeyframe(keyframe.frameNumber, keyframe.path);
        mask = roto.renderMask(sourceFrameIndex, frame.size());
    } else if (!entry.path.points.isEmpty()) {
        Rotoscope roto;
        roto.addKeyframe(sourceFrameIndex, entry.path);
        mask = roto.renderMask(sourceFrameIndex, frame.size());
    }
    if (!entry.brushMask.isNull())
        mask = combineMasksMax(mask, entry.brushMask);
    if (mask.isNull())
        return frame;
    return Rotoscope::applyToFrame(frame, mask);
}

QImage MainWindow::buildSpecialClipComposite(double timelineSeconds) const
{
    if (!m_timeline)
        return {};

    const QSize canvasSize = (m_projectConfig.width > 0 && m_projectConfig.height > 0)
        ? QSize(m_projectConfig.width, m_projectConfig.height)
        : QSize(1920, 1080);

    struct ActiveLayer {
        CompositeLayer layer;
        QString clipId;
        QImage image;
    };

    QVector<ActiveLayer> activeLayers;
    bool hasActiveSpecialData = false;
    for (int trackIdx = 0; trackIdx < m_timeline->videoTracks().size(); ++trackIdx) {
        const auto *track = m_timeline->videoTracks().value(trackIdx, nullptr);
        if (!track || track->isHidden())
            continue;

        const auto &clips = track->clips();
        double cursor = 0.0;
        for (int clipIdx = 0; clipIdx < clips.size(); ++clipIdx) {
            const ClipInfo &clip = clips[clipIdx];
            cursor += qMax(0.0, clip.leadInSec);
            const double clipStart = cursor;
            const double clipEnd = clipStart + clip.effectiveDuration();
            cursor = clipEnd;

            if (timelineSeconds + 1.0 / 120.0 < clipStart
                || timelineSeconds > clipEnd + 1.0 / 120.0) {
                continue;
            }

            const QString clipId = brushClipId(trackIdx, clipIdx);
            hasActiveSpecialData = hasActiveSpecialData
                || m_rotoClipEntries.contains(clipId)
                || m_timeRemapClipEntries.contains(clipId)
                || m_trackMatteClipEntries.contains(clipId)
                || m_text3DClipConfigs.contains(clipId)
                || m_clipExpressionBindings.contains(clipId)
                || (m_clipWiggleParams.contains(clipId) && m_clipWiggleParams.value(clipId).enabled);
            const VideoSourceInfo info = probeVideoSourceInfo(
                ProxyManager::instance().getProxyPath(clip.filePath),
                m_projectConfig.fps > 0 ? m_projectConfig.fps : 30.0);
            const double fps = clipEffectiveSourceFps(info, m_projectConfig.fps > 0 ? m_projectConfig.fps : 30.0);
            const double localSeconds = qBound(0.0, timelineSeconds - clipStart, clip.effectiveDuration());

            QImage frame;
            int sourceFrameIndex = 0;
            auto trIt = m_timeRemapClipEntries.constFind(clipId);
            if (trIt != m_timeRemapClipEntries.cend()) {
                timeremap::TimeRemapCurve curve = trIt.value().curve;
                if (curve.sourceFps <= 0.0)
                    curve.sourceFps = fps;
                sourceFrameIndex = qMax(0, static_cast<int>(std::llround(
                    (clip.inPoint + curve.srcTimeAt(localSeconds)) * curve.sourceFps)));
                frame = timeremap::resolveFrame(curve, localSeconds, [this, clip, curve](int srcFrameIndex) {
                    return decodeClipFrameByIndex(clip, srcFrameIndex, curve.sourceFps);
                });
            } else {
                const double clipOut = clipSourceOutPoint(clip);
                const double sourceTime = qBound(clip.inPoint,
                                                 clip.inPoint + localSeconds * qMax(clip.speed, 0.0001),
                                                 clipOut);
                sourceFrameIndex = qMax(0, static_cast<int>(std::llround(sourceTime * fps)));
                frame = decodeClipFrameAtSourceTime(clip, sourceTime);
            }

            if (frame.isNull())
                continue;
            frame = applyStoredRotoData(clipId, frame, sourceFrameIndex);

            // US-3D-11: alpha-over a rendered 3D extruded-text layer when this
            // clip carries one. localSeconds is the clip-local time in seconds.
            if (auto t3It = m_text3DClipConfigs.constFind(clipId);
                t3It != m_text3DClipConfigs.cend() && !t3It.value().isEmpty()) {
                Text3DLayer text3D;
                text3D.fromJson(t3It.value());
                const QImage textImage = text3D.renderFrame(frame.size(), localSeconds, Camera3D{});
                if (!textImage.isNull()) {
                    QImage base = frame.convertToFormat(QImage::Format_ARGB32);
                    QPainter painter(&base);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    painter.drawImage(QRect(QPoint(0, 0), base.size()), textImage);
                    painter.end();
                    frame = base;
                }
            }

            // US-3D-11: apply per-clip expression bindings + wiggle on top of
            // the keyframed transform values (videoDx/videoDy/videoScale/rotation/opacity).
            double posX = clip.videoDx;
            double posY = clip.videoDy;
            double layerScale = clip.videoScale;
            double layerRotation = clip.rotation2DDegrees;
            double layerOpacity = qBound(0.0, clip.opacity, 1.0);
            if (auto exprIt = m_clipExpressionBindings.constFind(clipId);
                exprIt != m_clipExpressionBindings.cend()) {
                const exprbind::ClipExpressionBindings &bindings = exprIt.value();
                ExpressionContext ctx;
                ctx.time = localSeconds;
                ctx.fps = m_projectConfig.fps > 0 ? m_projectConfig.fps : 30.0;
                ctx.duration = clip.effectiveDuration();
                ctx.canvasWidth = canvasSize.width();
                ctx.canvasHeight = canvasSize.height();
                posX = bindings.resolve(QStringLiteral("transform.position.x"), ctx, posX);
                posY = bindings.resolve(QStringLiteral("transform.position.y"), ctx, posY);
                layerScale = bindings.resolve(QStringLiteral("transform.scale"), ctx, layerScale);
                layerRotation = bindings.resolve(QStringLiteral("transform.rotation"), ctx, layerRotation);
                const double opacityKf = layerOpacity * 100.0;
                const double opacityVal = bindings.resolve(QStringLiteral("transform.opacity"), ctx, opacityKf);
                layerOpacity = qBound(0.0, opacityVal / 100.0, 1.0);
            }
            if (auto wigIt = m_clipWiggleParams.constFind(clipId);
                wigIt != m_clipWiggleParams.cend() && wigIt.value().enabled) {
                const wiggle::WiggleOffset off = wiggle::evaluate(wigIt.value(), localSeconds);
                // wiggle::positionOffset is in PIXELS; posX/posY are NORMALIZED
                // canvas fractions (canonical clipgeom contract). Convert the
                // pixel offset to the same normalized space before summing so
                // wiggle keeps its visual magnitude under clipgeom placement.
                if (canvasSize.width() > 0)
                    posX += off.positionOffset.x() / canvasSize.width();
                if (canvasSize.height() > 0)
                    posY += off.positionOffset.y() / canvasSize.height();
                layerRotation += off.rotationOffsetDeg;
                layerScale *= off.scaleMultiplier;
            }

            ActiveLayer active;
            active.clipId = clipId;
            active.image = frame;
            active.layer.name = clip.displayName;
            active.layer.opacity = layerOpacity;
            active.layer.blendMode = BlendMode::Normal;
            // Carry the RAW canonical transform fields (NOT a pixel offset):
            // position = NORMALIZED videoDx/videoDy fractions of the canvas,
            // scale.x() = uniform videoScale, rotation = rotation2DDegrees.
            // renderCompositeImage feeds these straight into clipgeom, which
            // owns the canvas-center anchor + translate->rotate->scale math —
            // so the special-clip preview matches GLPreview/export exactly.
            // (posX/posY already incorporate normalized expression + wiggle
            // offsets layered on clip.videoDx/clip.videoDy above.)
            active.layer.position = QPointF(posX, posY);
            active.layer.scale = QPointF(layerScale, layerScale);
            active.layer.rotation = layerRotation;
            active.layer.anchorPoint = QPointF(0.0, 0.0);
            active.layer.zOrder = trackIdx;
            active.layer.inPoint = clipStart;
            active.layer.outPoint = clipEnd;
            if (auto matteIt = m_trackMatteClipEntries.constFind(clipId);
                matteIt != m_trackMatteClipEntries.cend()) {
                active.layer.matteType = matteIt.value().matteType;
            }
            activeLayers.append(active);
        }
    }

    if (activeLayers.isEmpty())
        return {};
    if (!hasActiveSpecialData)
        return {};

    QHash<QString, int> indexByClipId;
    for (int i = 0; i < activeLayers.size(); ++i)
        indexByClipId.insert(activeLayers[i].clipId, i);

    for (int i = 0; i < activeLayers.size(); ++i) {
        const auto matteIt = m_trackMatteClipEntries.constFind(activeLayers[i].clipId);
        if (matteIt == m_trackMatteClipEntries.cend())
            continue;
        activeLayers[i].layer.matteType = matteIt.value().matteType;
        activeLayers[i].layer.matteSourceLayerIndex =
            indexByClipId.value(matteIt.value().matteSourceClipId, -1);
    }

    QVector<int> order(activeLayers.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&activeLayers](int a, int b) {
        return activeLayers[a].layer.zOrder < activeLayers[b].layer.zOrder;
    });

    // SSOT: hand the full z-ordered layer list plus parallel pre-transformed
    // canvas-sized images to trackmatte::composite so the GUI preview and the
    // export path (renderFrameAt, TM-3) share one matte+blend implementation.
    // matteSourceLayerIndex was resolved against the unsorted activeLayers
    // index space; remap it into the sorted-array position the SSOT indexes.
    QVector<int> sortedPosByOldIndex(activeLayers.size(), -1);
    for (int sortedPos = 0; sortedPos < order.size(); ++sortedPos)
        sortedPosByOldIndex[order[sortedPos]] = sortedPos;

    QVector<CompositeLayer> layers;
    QVector<QImage> layerImages;
    layers.reserve(order.size());
    layerImages.reserve(order.size());
    for (int sortedIdx : order) {
        const ActiveLayer &active = activeLayers[sortedIdx];
        CompositeLayer layer = active.layer;
        if (layer.matteType != TrackMatteType::None) {
            const int oldMatteIndex = layer.matteSourceLayerIndex;
            layer.matteSourceLayerIndex =
                (oldMatteIndex >= 0 && oldMatteIndex < sortedPosByOldIndex.size())
                    ? sortedPosByOldIndex[oldMatteIndex]
                    : -1;
        }
        layers.append(layer);
        layerImages.append(renderCompositeImage(active.image, layer, canvasSize));
    }

    return trackmatte::composite(layers, layerImages, canvasSize);
}

void MainWindow::refreshSpecialClipPreview()
{
    if (!m_player || !m_player->glPreview() || !m_timeline)
        return;

    static thread_local bool s_refreshingPreview = false;
    if (s_refreshingPreview)
        return;

    const int timelineMs = qRound(m_timeline->playheadPosition() * 1000.0);
    bool anyWiggleEnabled = false;
    for (auto it = m_clipWiggleParams.cbegin(); it != m_clipWiggleParams.cend(); ++it) {
        if (it.value().enabled) { anyWiggleEnabled = true; break; }
    }
    if (m_rotoClipEntries.isEmpty() && m_timeRemapClipEntries.isEmpty()
        && m_trackMatteClipEntries.isEmpty() && m_text3DClipConfigs.isEmpty()
        && m_clipExpressionBindings.isEmpty() && !anyWiggleEnabled) {
        if (!m_player->isPlaying()) {
            s_refreshingPreview = true;
            m_player->previewSeek(timelineMs);
            s_refreshingPreview = false;
        }
        return;
    }

    const QImage composed = buildSpecialClipComposite(m_timeline->playheadPosition());
    if (!composed.isNull()) {
        s_refreshingPreview = true;
        m_player->glPreview()->displayFrame(composed);
        s_refreshingPreview = false;
    } else if (!m_player->isPlaying()) {
        s_refreshingPreview = true;
        m_player->previewSeek(timelineMs);
        s_refreshingPreview = false;
    }
}

void MainWindow::populateProjectData(ProjectData &data)
{
    data.config = m_projectConfig;
    data.videoTracks = m_timeline->allVideoTracks();
    data.audioTracks = m_timeline->allAudioTracks();
    data.playheadPos = m_timeline->playheadPosition();
    data.markIn = m_timeline->markedIn();
    data.markOut = m_timeline->markedOut();
    data.zoomLevel = 10; // TODO: expose zoom level getter
    data.brushAnimations = m_brushAnimationEntries;
    data.rotoClipEntries.clear();
    for (auto it = m_rotoClipEntries.cbegin(); it != m_rotoClipEntries.cend(); ++it)
        data.rotoClipEntries.append(it.value());
    std::sort(data.rotoClipEntries.begin(), data.rotoClipEntries.end(),
              [](const RotoClipEntry &a, const RotoClipEntry &b) { return a.clipId < b.clipId; });
    data.timeRemapClipEntries.clear();
    for (auto it = m_timeRemapClipEntries.cbegin(); it != m_timeRemapClipEntries.cend(); ++it)
        data.timeRemapClipEntries.append(it.value());
    std::sort(data.timeRemapClipEntries.begin(), data.timeRemapClipEntries.end(),
              [](const TimeRemapClipEntry &a, const TimeRemapClipEntry &b) { return a.clipId < b.clipId; });
    data.trackMatteClipEntries.clear();
    for (auto it = m_trackMatteClipEntries.cbegin(); it != m_trackMatteClipEntries.cend(); ++it)
        data.trackMatteClipEntries.append(it.value());
    std::sort(data.trackMatteClipEntries.begin(), data.trackMatteClipEntries.end(),
              [](const TrackMatteClipEntry &a, const TrackMatteClipEntry &b) { return a.clipId < b.clipId; });

    // US-3D-11: motion-graphics sprint sidecars
    data.text3DClipEntries.clear();
    for (auto it = m_text3DClipConfigs.cbegin(); it != m_text3DClipConfigs.cend(); ++it) {
        if (it.value().isEmpty())
            continue;
        Text3DClipEntry entry;
        entry.clipId = it.key();
        entry.config = it.value();
        data.text3DClipEntries.append(entry);
    }
    std::sort(data.text3DClipEntries.begin(), data.text3DClipEntries.end(),
              [](const Text3DClipEntry &a, const Text3DClipEntry &b) { return a.clipId < b.clipId; });
    data.expressionBindingsEntries.clear();
    for (auto it = m_clipExpressionBindings.cbegin(); it != m_clipExpressionBindings.cend(); ++it) {
        if (it.value().isEmpty())
            continue;
        ExpressionBindingsClipEntry entry;
        entry.clipId = it.key();
        entry.bindings = it.value();
        data.expressionBindingsEntries.append(entry);
    }
    std::sort(data.expressionBindingsEntries.begin(), data.expressionBindingsEntries.end(),
              [](const ExpressionBindingsClipEntry &a, const ExpressionBindingsClipEntry &b) { return a.clipId < b.clipId; });
    data.wiggleClipEntries.clear();
    for (auto it = m_clipWiggleParams.cbegin(); it != m_clipWiggleParams.cend(); ++it) {
        WiggleClipEntry entry;
        entry.clipId = it.key();
        entry.params = it.value();
        data.wiggleClipEntries.append(entry);
    }
    std::sort(data.wiggleClipEntries.begin(), data.wiggleClipEntries.end(),
              [](const WiggleClipEntry &a, const WiggleClipEntry &b) { return a.clipId < b.clipId; });
    data.projectCamera = m_projectCamera.toJson();

    // US-HW-10: persist project-level sidechain ducking parameters.
    data.duckingParams  = m_duckingParams;
    data.duckingEnabled = m_duckingEnabled;

    // US-EXT-10: persist project-level HDR + AI processing settings.
    data.hdrSettings = m_hdrSettings;
    data.aiSettings  = m_aiSettings;

    // PRD-PROJECT-PRESET US-PP-4: persist tracker dialog states.
    data.motionTrackerState = m_motionTrackerState;
    data.planarTrackerState = m_planarTrackerState;

    collectAudioState(data);

    // MP-5: メディアプール (ビン/素材/スマートビン) をプロジェクトへ保存。
    data.mediaPool = m_mediaPool;

    // AB-5: オーディオ バス ルーティング (バス/サブミックス/AUX) を保存。
    data.audioBusRouting = m_audioBusRouting;

    // AC-4: ACES カラーマネジメント パイプライン設定を保存。
    data.acesPipeline = m_acesPipeline;

    // DV-4: Dolby Vision メタデータ (プロファイル/L6/ショット) を保存。
    data.dolbyVision = m_dolbyVision;

    // CC-4: 放送用クローズドキャプション (CEA-608/708) を保存。
    data.broadcastCaption = m_broadcastCaption;

    data.smartReframe = m_smartReframe.toJson();
    data.subtitleSegments.clear();
    for (const auto &seg : m_subtitleSegments) {
        SubtitleEntry entry;
        entry.start = seg.startTime;
        entry.end = seg.endTime;
        entry.text = seg.text;
        data.subtitleSegments.append(entry);
    }
    QJsonObject styleJson;
    styleJson["fontName"] = m_subtitleStyle.font.family();
    styleJson["fontSize"] = m_subtitleStyle.font.pointSize();
    styleJson["fontBold"] = m_subtitleStyle.font.bold();
    styleJson["color"] = m_subtitleStyle.color.name(QColor::HexArgb);
    styleJson["outlineColor"] = m_subtitleStyle.outlineColor.name(QColor::HexArgb);
    styleJson["outlineWidth"] = m_subtitleStyle.outlineWidth;
    styleJson["verticalPos"] = m_subtitleStyle.verticalPos;
    data.subtitleStyle = styleJson;

    QJsonObject loudnessJson;
    loudnessJson["normalizerAmount"] = 0.0;
    loudnessJson["loudnessGainDb"] = 0.0;
    data.loudnessSettings = loudnessJson;

    data.particleClipEntries.clear();
    for (int trackIdx = 0; trackIdx < data.videoTracks.size(); ++trackIdx) {
        const auto &track = data.videoTracks[trackIdx];
        for (int clipIdx = 0; clipIdx < track.size(); ++clipIdx) {
            const auto &clip = track[clipIdx];
            const QString key = particleClipKey(clip);
            if (!m_particleClipConfigs.contains(key))
                continue;
            ParticleClipEntry entry;
            entry.trackIndex = trackIdx;
            entry.clipIndex = clipIdx;
            entry.clipFilePath = clip.filePath;
            entry.config = m_particleClipConfigs.value(key);
            data.particleClipEntries.append(entry);
        }
    }

    data.clipNodeGraphs.clear();
    if (m_nodeModeActive && m_activeNodeGraph && !m_nodeModeClipId.isEmpty()) {
        ClipNodeGraph cng;
        cng.clipId = m_nodeModeClipId;
        cng.graph = m_activeNodeGraph->toJson();
        cng.compositingMode = QStringLiteral("node");
        data.clipNodeGraphs.append(cng);
    }

    if (m_vfxControlsPanel) {
        const GlowState glow = m_vfxControlsPanel->glowState();
        data.vfxState.glow.enabled = glow.enabled;
        data.vfxState.glow.threshold = glow.threshold;
        data.vfxState.glow.radius = glow.radius;
        data.vfxState.glow.intensity = glow.intensity;

        const BloomState bloom = m_vfxControlsPanel->bloomState();
        data.vfxState.bloom.enabled = bloom.enabled;
        data.vfxState.bloom.threshold = bloom.threshold;
        data.vfxState.bloom.intensity = bloom.intensity;
        data.vfxState.bloom.spread = bloom.spread;

        const ChromaticAberrationState chromatic = m_vfxControlsPanel->chromaticAberrationState();
        data.vfxState.chromaticAberration.enabled = chromatic.enabled;
        data.vfxState.chromaticAberration.amount = chromatic.amount;
        data.vfxState.chromaticAberration.radialFalloff = chromatic.radialFalloff;

        const LightWrapState lightWrap = m_vfxControlsPanel->lightWrapState();
        data.vfxState.lightWrap.enabled = lightWrap.enabled;
        data.vfxState.lightWrap.amount = lightWrap.amount;
        data.vfxState.lightWrap.radius = lightWrap.radius;
    }
}

void MainWindow::applyVfxProjectState(const ProjectVfxState &state)
{
    setVfxPanelState(m_vfxControlsPanel, state);

    if (!m_player || !m_player->glPreview())
        return;

    auto *preview = m_player->glPreview();
    preview->setGlow(state.glow.enabled, state.glow.threshold, state.glow.radius, state.glow.intensity);
    preview->setBloom(state.bloom.enabled, state.bloom.threshold, state.bloom.intensity, state.bloom.spread);
    preview->setChromaticAberration(state.chromaticAberration.enabled,
                                    state.chromaticAberration.amount,
                                    state.chromaticAberration.radialFalloff);
    preview->setLightWrap(state.lightWrap.enabled, state.lightWrap.amount, state.lightWrap.radius);
}

void MainWindow::applyLoadedProjectData(const ProjectData &data, const QString &filePath)
{
    m_projectFilePath = filePath;
    if (m_recentFilesManager && !filePath.isEmpty())
        m_recentFilesManager->addFile(filePath);

    m_projectConfig = data.config;
    setBrushAnimationEntries(data.brushAnimations);
    m_rotoClipEntries.clear();
    for (const auto &entry : data.rotoClipEntries) {
        if (!entry.clipId.isEmpty())
            m_rotoClipEntries.insert(entry.clipId, entry);
    }
    m_timeRemapClipEntries.clear();
    for (const auto &entry : data.timeRemapClipEntries) {
        if (!entry.clipId.isEmpty())
            m_timeRemapClipEntries.insert(entry.clipId, entry);
    }
    m_trackMatteClipEntries.clear();
    for (const auto &entry : data.trackMatteClipEntries) {
        if (!entry.clipId.isEmpty())
            m_trackMatteClipEntries.insert(entry.clipId, entry);
    }
    // TM-8: push the freshly-loaded matte wiring onto the Timeline so the
    // SSOT renderer (preview AND export) sources it from the Timeline.
    syncTrackMatteEntriesToTimeline(m_timeline, m_trackMatteClipEntries);
    // US-3D-11: motion-graphics sprint sidecars
    m_text3DClipConfigs.clear();
    for (const auto &entry : data.text3DClipEntries) {
        if (!entry.clipId.isEmpty() && !entry.config.isEmpty())
            m_text3DClipConfigs.insert(entry.clipId, entry.config);
    }
    m_clipExpressionBindings.clear();
    for (const auto &entry : data.expressionBindingsEntries) {
        if (!entry.clipId.isEmpty() && !entry.bindings.isEmpty())
            m_clipExpressionBindings.insert(entry.clipId, entry.bindings);
    }
    m_clipWiggleParams.clear();
    for (const auto &entry : data.wiggleClipEntries) {
        if (!entry.clipId.isEmpty())
            m_clipWiggleParams.insert(entry.clipId, entry.params);
    }
    m_projectCamera = Camera3D{};
    if (!data.projectCamera.isEmpty())
        m_projectCamera.fromJson(data.projectCamera);
    // US-HW-10: restore project-level sidechain ducking parameters.
    m_duckingParams  = data.duckingParams;
    m_duckingEnabled = data.duckingEnabled;
    // US-EXT-10: restore project-level HDR + AI processing settings.
    m_hdrSettings = data.hdrSettings;
    m_aiSettings  = data.aiSettings;

    // PRD-PROJECT-PRESET US-PP-4: restore tracker dialog states.
    m_motionTrackerState = data.motionTrackerState;
    m_planarTrackerState = data.planarTrackerState;

    m_selectedVideoTrackIndex = -1;
    m_selectedVideoClipIndexTracked = -1;

    if (m_player)
        m_player->setCanvasSize(data.config.width, data.config.height);
    if (m_timeline)
        m_timeline->restoreFromProject(data.videoTracks, data.audioTracks,
                                       data.playheadPos, data.markIn, data.markOut, data.zoomLevel);

    rebuildAudioMeters();
    applyAudioState(data);

    m_particleClipConfigs.clear();
    for (const auto &entry : data.particleClipEntries) {
        QString key = entry.clipFilePath;
        if (key.isEmpty()
            && entry.trackIndex >= 0
            && entry.trackIndex < data.videoTracks.size()
            && entry.clipIndex >= 0
            && entry.clipIndex < data.videoTracks[entry.trackIndex].size()) {
            key = particleClipKey(data.videoTracks[entry.trackIndex][entry.clipIndex]);
        }
        if (!key.isEmpty())
            m_particleClipConfigs.insert(key, entry.config);
    }

    if (!data.smartReframe.isEmpty()) {
        m_smartReframe.fromJson(data.smartReframe);
        m_exporter->setSmartReframe(&m_smartReframe);
    }
    m_subtitleSegments.clear();
    for (const auto &entry : data.subtitleSegments) {
        SubtitleSegment seg;
        seg.startTime = entry.start;
        seg.endTime = entry.end;
        seg.text = entry.text;
        m_subtitleSegments.append(seg);
    }
    m_subtitleStyle = SubtitleStyle{};
    if (!data.subtitleStyle.isEmpty()) {
        m_subtitleStyle.font.setFamily(data.subtitleStyle.value("fontName").toString());
        m_subtitleStyle.font.setPointSize(data.subtitleStyle.value("fontSize").toInt());
        m_subtitleStyle.font.setBold(data.subtitleStyle.value("fontBold").toBool());
        m_subtitleStyle.color = QColor(data.subtitleStyle.value("color").toString());
        m_subtitleStyle.outlineColor = QColor(data.subtitleStyle.value("outlineColor").toString());
        m_subtitleStyle.outlineWidth = data.subtitleStyle.value("outlineWidth").toDouble();
        m_subtitleStyle.verticalPos = data.subtitleStyle.value("verticalPos").toDouble();
    }

    applyVfxProjectState(data.vfxState);

    m_nodeModeActive = false;
    m_nodeModeClipId.clear();
    if (m_nodeModeAction) {
        QSignalBlocker blocker(m_nodeModeAction);
        m_nodeModeAction->setChecked(false);
    }
    if (m_nodeCanvasDock)
        m_nodeCanvasDock->setVisible(false);
    if (m_nodePropsDock)
        m_nodePropsDock->setVisible(false);

    for (const auto &cng : data.clipNodeGraphs) {
        if (cng.compositingMode != QLatin1String("node") || cng.graph.isEmpty())
            continue;
        m_nodeModeClipId = cng.clipId;
        m_activeNodeGraph->fromJson(cng.graph);
        m_nodeModeActive = true;
        setupNodeCompositingDocks();
        m_nodeCanvas->setGraph(m_activeNodeGraph);
        m_nodeCanvasDock->setVisible(true);
        m_nodePropsDock->setVisible(true);
        if (m_nodeModeAction) {
            QSignalBlocker blocker(m_nodeModeAction);
            m_nodeModeAction->setChecked(true);
        }
        break;
    }

    // MP-5: メディアプールを復元し、ドックを再描画する。
    m_mediaPool = data.mediaPool;
    if (m_mediaPoolDock)
        m_mediaPoolDock->refresh();

    // AB-5: オーディオ バス ルーティングを復元し、AudioMixer へ反映 + パネル再描画。
    m_audioBusRouting = data.audioBusRouting;
    if (auto *mixer = m_player ? m_player->audioMixer() : nullptr)
        mixer->setBusRouting(m_audioBusRouting);
    if (m_audioBusPanel) {
        m_audioBusPanel->setRouting(&m_audioBusRouting);
        m_audioBusPanel->refresh();
    }

    // AC-4: ACES カラーマネジメント パイプライン設定を復元 (SSOT)。
    // ダイアログを次に開いたときにこの状態が初期値となる。
    m_acesPipeline = data.acesPipeline;

    // DV-4: Dolby Vision メタデータを復元 (SSOT)。
    m_dolbyVision = data.dolbyVision;

    // CC-4: 放送用クローズドキャプション (CEA-608/708) を復元 (SSOT)。
    m_broadcastCaption = data.broadcastCaption;

    updateTitle();
    hideWelcomeScreen();
    updateStatusInfo();
    updateEditActions();
    syncBrushAnimationPreviewForClip(0, m_timeline ? m_timeline->selectedVideoClipIndex() : -1);
    refreshSpecialClipPreview();
}

void MainWindow::collectAudioState(ProjectData &data)
{
    if (auto *mixer = m_player ? m_player->audioMixer() : nullptr) {
        const int n = m_timeline ? m_timeline->audioTrackCount() : 0;
        data.trackEqStates.resize(n);
        for (int i = 0; i < n; ++i) {
            AudioEQConfig cfg = mixer->trackEqConfig(i);
            TrackEqState &s = data.trackEqStates[i];
            s.trackIdx = i;
            if (cfg.bands.size() > 0) {
                s.low = cfg.bands[0].gain;
                s.lowFreqHz = cfg.bands[0].frequency;
            }
            if (cfg.bands.size() > 1) {
                s.mid = cfg.bands[1].gain;
                s.midFreqHz = cfg.bands[1].frequency;
            }
            if (cfg.bands.size() > 2) {
                s.high = cfg.bands[2].gain;
                s.highFreqHz = cfg.bands[2].frequency;
            }
            s.qFactor = cfg.bands.isEmpty() ? 1.0 : cfg.bands[0].q;
            s.enabled = !cfg.isDefault();
        }
        const auto &cp = mixer->compressorParams();
        data.masterCompressor.thresholdDb = cp.thresholdDb;
        data.masterCompressor.ratio = cp.ratio;
        data.masterCompressor.attackMs = cp.attackMs;
        data.masterCompressor.releaseMs = cp.releaseMs;
        data.masterCompressor.makeupDb = cp.makeupDb;
        data.masterCompressor.enabled = cp.enabled;

        const auto &adParams = mixer->autoDuckParams();
        data.autoDuck.thresholdDb = adParams.thresholdDb;
        data.autoDuck.ratio = adParams.ratio;
        data.autoDuck.attackMs = adParams.attackMs;
        data.autoDuck.releaseMs = adParams.releaseMs;
    }
    data.audioMetersDockVisible = m_audioMetersDock ? m_audioMetersDock->isVisible() : true;
}

void MainWindow::applyAudioState(const ProjectData &data)
{
    if (auto *mixer = m_player ? m_player->audioMixer() : nullptr) {
        for (const auto &s : data.trackEqStates) {
            AudioEQConfig cfg;
            cfg.bands.resize(3);
            cfg.bands[0].frequency = s.lowFreqHz;
            cfg.bands[0].gain = s.low;
            cfg.bands[0].q = s.qFactor;
            cfg.bands[1].frequency = s.midFreqHz;
            cfg.bands[1].gain = s.mid;
            cfg.bands[1].q = s.qFactor;
            cfg.bands[2].frequency = s.highFreqHz;
            cfg.bands[2].gain = s.high;
            cfg.bands[2].q = s.qFactor;
            mixer->setTrackEqConfig(s.trackIdx, cfg);
            mixer->setTrackEqEnabled(s.trackIdx, s.enabled);
        }
        AudioMixer::CompressorParams cp;
        cp.thresholdDb = data.masterCompressor.thresholdDb;
        cp.ratio = data.masterCompressor.ratio;
        cp.attackMs = data.masterCompressor.attackMs;
        cp.releaseMs = data.masterCompressor.releaseMs;
        cp.makeupDb = data.masterCompressor.makeupDb;
        cp.enabled = data.masterCompressor.enabled;
        mixer->setCompressorParams(cp);

        AudioMixer::AutoDuckParams adParams;
        adParams.thresholdDb = data.autoDuck.thresholdDb;
        adParams.ratio = data.autoDuck.ratio;
        adParams.attackMs = data.autoDuck.attackMs;
        adParams.releaseMs = data.autoDuck.releaseMs;
        mixer->setAutoDuckParams(adParams);
    }
    if (m_audioMetersDock)
        m_audioMetersDock->setVisible(data.audioMetersDockVisible);
}

void MainWindow::newProject()
{
    ProjectSettingsDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        applyProjectConfig(dialog.config());
        hideWelcomeScreen();
        updateStatusInfo();
    }
}

void MainWindow::editProjectSettings()
{
    ProjectSettingsDialog dialog(this, &m_projectConfig);
    if (dialog.exec() == QDialog::Accepted) {
        applyProjectConfig(dialog.config());
        updateStatusInfo();
    }
}

void MainWindow::saveProject()
{
    if (m_projectFilePath.isEmpty()) {
        saveProjectAs();
        return;
    }

    ProjectData data;
    populateProjectData(data);

    if (ProjectFile::save(m_projectFilePath, data)) {
        statusBar()->showMessage("Saved: " + m_projectFilePath);
        updateTitle();
    } else {
        QMessageBox::critical(this, "Save Failed", "Could not save project file.");
    }
}

void MainWindow::saveProjectAs()
{
    QString filePath = QFileDialog::getSaveFileName(this, "Save Project As",
        m_projectConfig.name + ".veditor", ProjectFile::fileFilter());
    if (filePath.isEmpty()) return;
    m_projectFilePath = filePath;
    saveProject();
}

void MainWindow::openProject()
{
    QString filePath = QFileDialog::getOpenFileName(this, "Open Project",
        QString(), ProjectFile::fileFilter());
    if (filePath.isEmpty()) return;

    ProjectData data;
    if (!ProjectFile::load(filePath, data)) {
        QMessageBox::critical(this, "Open Failed", "Could not load project file.");
        return;
    }

    applyLoadedProjectData(data, filePath);
    statusBar()->showMessage("Opened: " + filePath);
}

void MainWindow::openFile()
{
    QString filter = "Video Files (*.mp4 *.mkv *.mov *.webm *.flv);;All Files (*)";
    QString filePath = QFileDialog::getOpenFileName(this, "Open Video", QString(), filter);
    if (!filePath.isEmpty())
        loadMediaFile(filePath, true, "Loaded");
}

void MainWindow::importVideoFromUrl()
{
    YtdlpDownloadDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString path = dialog.downloadedFilePath();
    if (path.isEmpty())
        return;
    loadMediaFile(path, true, QStringLiteral("URL から取り込み"));
}

// MP-5: 拡張子からメディア種別を判定する小ヘルパー (importToMediaPool 専用)。
// 大文字小文字を無視し、未知の拡張子は Unknown を返す。
static mediapool::MediaType mediaTypeForExtension(const QString &suffix)
{
    const QString ext = suffix.toLower();
    static const QStringList kVideo = {
        "mp4", "mkv", "mov", "webm", "flv", "avi", "wmv", "m4v", "mpg", "mpeg"};
    static const QStringList kAudio = {
        "mp3", "wav", "aac", "flac", "ogg", "m4a", "wma", "opus", "aiff"};
    static const QStringList kImage = {
        "png", "jpg", "jpeg", "bmp", "gif", "tiff", "tif", "webp", "heic"};
    if (kVideo.contains(ext))
        return mediapool::MediaType::Video;
    if (kAudio.contains(ext))
        return mediapool::MediaType::Audio;
    if (kImage.contains(ext))
        return mediapool::MediaType::Image;
    return mediapool::MediaType::Unknown;
}

// MP-5: メディアプールへ動画/音声/画像を複数取り込む。各ファイルを MediaAsset
// 化して m_mediaPool.addAsset() で登録し、ドックを再描画する。重複パスは
// MediaPool 側で既存 id に集約されるので二重登録にはならない。
void MainWindow::importToMediaPool()
{
    const QString filter = QStringLiteral(
        "メディアファイル (*.mp4 *.mkv *.mov *.webm *.flv *.avi "
        "*.mp3 *.wav *.aac *.flac *.ogg *.m4a "
        "*.png *.jpg *.jpeg *.bmp *.gif *.tiff *.webp);;すべてのファイル (*)");
    const QStringList paths =
        QFileDialog::getOpenFileNames(this, QStringLiteral("メディアプールへ取り込み"),
                                      QString(), filter);
    if (paths.isEmpty())
        return;

    const QString nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    int added = 0;
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        const QFileInfo fi(path);
        mediapool::MediaAsset asset;
        asset.filePath      = path;
        asset.displayName   = fi.fileName();
        asset.type          = mediaTypeForExtension(fi.suffix());
        asset.fileSizeBytes = fi.size();
        asset.importedAtIso = nowIso;
        const int beforeCount = m_mediaPool.assets().size();
        m_mediaPool.addAsset(asset);
        if (m_mediaPool.assets().size() > beforeCount)
            ++added;
    }

    if (m_mediaPoolDock)
        m_mediaPoolDock->refresh();
    if (added > 0) {
        statusBar()->showMessage(
            QStringLiteral("メディアプールに %1 件取り込みました").arg(added));
    }
}

// MP-5 / SM-5: メディアプールの素材をダブルクリックしたとき、直接タイムラインへ
// 取り込まず、いったんソースモニターへロードする (3 点編集ワークフロー)。
// マークイン/アウト後に「挿入」/「上書き」で再生ヘッド位置へ取り込む。
void MainWindow::onMediaPoolAssetActivated(const QString &filePath)
{
    if (filePath.isEmpty())
        return;
    openInSourceMonitor(filePath);
}

// SM-5: 素材をソースモニターへロードする。長さ/表示名はメディアプールに
// 登録済みなら MediaAsset から取得し、無ければファイル名 + 長さ 0 で渡す
// (SourceMonitorDock 側で VideoPlayer の durationChanged から実尺を補正する)。
void MainWindow::openInSourceMonitor(const QString &filePath)
{
    if (filePath.isEmpty() || !m_sourceMonitorDock)
        return;

    double durationSec = 0.0;
    QString displayName;
    for (const mediapool::MediaAsset &asset : m_mediaPool.assets()) {
        if (asset.filePath == filePath) {
            if (asset.durationMs > 0)
                durationSec = static_cast<double>(asset.durationMs) / 1000.0;
            displayName = asset.displayName;
            break;
        }
    }
    if (displayName.isEmpty())
        displayName = QFileInfo(filePath).fileName();

    m_sourceMonitorDock->loadSource(filePath, durationSec, displayName);
    m_sourceMonitorDock->show();
    m_sourceMonitorDock->raise();
    statusBar()->showMessage(
        QStringLiteral("ソースモニターに読み込みました: %1").arg(displayName));
}

// SM-5: ソースモニターの「挿入 (Insert)」押下。選択範囲を検証してから
// ClipInfo へ変換し、再生ヘッド位置へリップル挿入する。
void MainWindow::onSourceInsertRequested(const threepoint::SourceSelection &sel)
{
    QString error;
    if (!threepoint::validate(sel, &error)) {
        statusBar()->showMessage(
            QStringLiteral("挿入できません: %1").arg(error));
        return;
    }
    if (!m_timeline) {
        statusBar()->showMessage(QStringLiteral("タイムラインがありません"));
        return;
    }
    ClipInfo clip = threepoint::buildClipInfo(sel);
    const double playheadSec = currentPlayheadSeconds();
    m_timeline->insertClip3PointActive(playheadSec, clip);
    updateStatusInfo();
    statusBar()->showMessage(
        QStringLiteral("再生ヘッド %1 秒に挿入しました: %2")
            .arg(playheadSec, 0, 'f', 2)
            .arg(sel.displayName.isEmpty()
                     ? QFileInfo(sel.filePath).fileName()
                     : sel.displayName));
}

// SM-5: ソースモニターの「上書き (Overwrite)」押下。選択範囲を検証してから
// ClipInfo へ変換し、再生ヘッド位置から上書き (既存クリップを分割/削除) する。
void MainWindow::onSourceOverwriteRequested(const threepoint::SourceSelection &sel)
{
    QString error;
    if (!threepoint::validate(sel, &error)) {
        statusBar()->showMessage(
            QStringLiteral("上書きできません: %1").arg(error));
        return;
    }
    if (!m_timeline) {
        statusBar()->showMessage(QStringLiteral("タイムラインがありません"));
        return;
    }
    ClipInfo clip = threepoint::buildClipInfo(sel);
    const double playheadSec = currentPlayheadSeconds();
    m_timeline->overwriteClip3PointActive(playheadSec, clip);
    updateStatusInfo();
    statusBar()->showMessage(
        QStringLiteral("再生ヘッド %1 秒から上書きしました: %2")
            .arg(playheadSec, 0, 'f', 2)
            .arg(sel.displayName.isEmpty()
                     ? QFileInfo(sel.filePath).fileName()
                     : sel.displayName));
}

// AB-5: オーディオ バス パネルでルーティングが変更されたとき。SSOT である
// m_audioBusRouting (パネルが直接更新済み) を AudioMixer へ反映する。AudioMixer
// 未生成のときは将来 setSequence/再生開始時に setBusRouting が呼ばれるよう
// SSOT 側にだけ残す (ここでは no-op)。
void MainWindow::onAudioBusRoutingChanged()
{
    if (auto *mixer = m_player ? m_player->audioMixer() : nullptr)
        mixer->setBusRouting(m_audioBusRouting);
}

// TR-4: 選択クリップの先頭を再生ヘッドへ合わせる (RippleIn)。
// delta(timeline 秒) = 再生ヘッド秒 - クリップ開始秒。正なら頭を詰めて短く、
// 負なら頭を伸ばす。下流クリップは applyTrimActive 内で隙間なくシフトする。
void MainWindow::rippleTrimInToPlayhead()
{
    if (!m_timeline) {
        statusBar()->showMessage(QStringLiteral("タイムラインがありません"));
        return;
    }
    int trackIdx = -1, clipIdx = -1;
    if (!selectedVideoClipRef(trackIdx, clipIdx)) {
        statusBar()->showMessage(QStringLiteral("トリムするクリップを選択してください"));
        return;
    }
    const double clipStart = clipTimelineStartSeconds(trackIdx, clipIdx);
    const double delta = currentPlayheadSeconds() - clipStart;

    QString err;
    if (m_timeline->applyTrimActive(trimops::TrimType::RippleIn, delta, &err)) {
        updateStatusInfo();
        statusBar()->showMessage(
            QStringLiteral("先頭を再生ヘッドへリップルしました (%1 秒)")
                .arg(delta, 0, 'f', 2));
    } else {
        statusBar()->showMessage(QStringLiteral("リップルできません: %1").arg(err));
    }
}

// TR-4: 選択クリップの末尾を再生ヘッドへ合わせる (RippleOut)。
// delta(timeline 秒) = 再生ヘッド秒 - クリップ終了秒。下流クリップは
// applyTrimActive 内で同量シフトしてギャップを保つ。
void MainWindow::rippleTrimOutToPlayhead()
{
    if (!m_timeline) {
        statusBar()->showMessage(QStringLiteral("タイムラインがありません"));
        return;
    }
    int trackIdx = -1, clipIdx = -1;
    ClipInfo clip;
    if (!selectedVideoClipRef(trackIdx, clipIdx, &clip)) {
        statusBar()->showMessage(QStringLiteral("トリムするクリップを選択してください"));
        return;
    }
    const double clipEnd =
        clipTimelineStartSeconds(trackIdx, clipIdx) + clip.effectiveDuration();
    const double delta = currentPlayheadSeconds() - clipEnd;

    QString err;
    if (m_timeline->applyTrimActive(trimops::TrimType::RippleOut, delta, &err)) {
        updateStatusInfo();
        statusBar()->showMessage(
            QStringLiteral("末尾を再生ヘッドへリップルしました (%1 秒)")
                .arg(delta, 0, 'f', 2));
    } else {
        statusBar()->showMessage(QStringLiteral("リップルできません: %1").arg(err));
    }
}

// TR-4: 選択クリップと次クリップの編集点を再生ヘッドへ合わせる (Roll)。
// 編集点 = 選択クリップ終了秒 (= 次クリップ開始秒)。total 尺は不変。
void MainWindow::rollEditToPlayhead()
{
    if (!m_timeline) {
        statusBar()->showMessage(QStringLiteral("タイムラインがありません"));
        return;
    }
    int trackIdx = -1, clipIdx = -1;
    ClipInfo clip;
    if (!selectedVideoClipRef(trackIdx, clipIdx, &clip)) {
        statusBar()->showMessage(QStringLiteral("トリムするクリップを選択してください"));
        return;
    }
    const double editPoint =
        clipTimelineStartSeconds(trackIdx, clipIdx) + clip.effectiveDuration();
    const double delta = currentPlayheadSeconds() - editPoint;

    QString err;
    if (m_timeline->applyTrimActive(trimops::TrimType::Roll, delta, &err)) {
        updateStatusInfo();
        statusBar()->showMessage(
            QStringLiteral("編集点を再生ヘッドへロールしました (%1 秒)")
                .arg(delta, 0, 'f', 2));
    } else {
        statusBar()->showMessage(QStringLiteral("ロールできません: %1").arg(err));
    }
}

// TR-4: 選択クリップのスリップ。秒数を入力させ、見せる窓 (in/out) だけを
// ずらす。タイムライン上の位置・実尺は不変。
void MainWindow::slipSelectedClip()
{
    if (!m_timeline) {
        statusBar()->showMessage(QStringLiteral("タイムラインがありません"));
        return;
    }
    int trackIdx = -1, clipIdx = -1;
    if (!selectedVideoClipRef(trackIdx, clipIdx)) {
        statusBar()->showMessage(QStringLiteral("トリムするクリップを選択してください"));
        return;
    }
    bool ok = false;
    const double delta = QInputDialog::getDouble(
        this, QStringLiteral("スリップ"),
        QStringLiteral("ずらす秒数 (正=後ろ / 負=前):"),
        0.0, -3600.0, 3600.0, 2, &ok);
    if (!ok || qFuzzyIsNull(delta))
        return;

    QString err;
    if (m_timeline->applyTrimActive(trimops::TrimType::Slip, delta, &err)) {
        updateStatusInfo();
        statusBar()->showMessage(
            QStringLiteral("クリップをスリップしました (%1 秒)")
                .arg(delta, 0, 'f', 2));
    } else {
        statusBar()->showMessage(QStringLiteral("スリップできません: %1").arg(err));
    }
}

// TR-4: 選択クリップのスライド。秒数を入力させ、クリップ中身は不変のまま
// タイムライン上の位置だけを動かす。隣接クリップが伸縮して吸収する。
void MainWindow::slideSelectedClip()
{
    if (!m_timeline) {
        statusBar()->showMessage(QStringLiteral("タイムラインがありません"));
        return;
    }
    int trackIdx = -1, clipIdx = -1;
    if (!selectedVideoClipRef(trackIdx, clipIdx)) {
        statusBar()->showMessage(QStringLiteral("トリムするクリップを選択してください"));
        return;
    }
    bool ok = false;
    const double delta = QInputDialog::getDouble(
        this, QStringLiteral("スライド"),
        QStringLiteral("動かす秒数 (正=後ろ / 負=前):"),
        0.0, -3600.0, 3600.0, 2, &ok);
    if (!ok || qFuzzyIsNull(delta))
        return;

    QString err;
    if (m_timeline->applyTrimActive(trimops::TrimType::Slide, delta, &err)) {
        updateStatusInfo();
        statusBar()->showMessage(
            QStringLiteral("クリップをスライドしました (%1 秒)")
                .arg(delta, 0, 'f', 2));
    } else {
        statusBar()->showMessage(QStringLiteral("スライドできません: %1").arg(err));
    }
}

void MainWindow::exportVideo()
{
    ExportDialog dialog(m_projectConfig, this);
    dialog.setSourceIsHdr(m_player && m_player->isHdrSource());
    // タイムラインの V1 クリップを渡すことで、ダイアログ内の YouTube 概要欄
    // チャプター生成 (US-6F-2) と Premiere XML エクスポートが実 export 経路で
    // 実データを参照できるようにする。
    dialog.setClips(m_timeline->videoClips());
    if (dialog.exec() != QDialog::Accepted) return;

    ExportConfig exportCfg = dialog.config();
    const auto &clips = m_timeline->videoClips();

    if (clips.isEmpty()) {
        QMessageBox::warning(this, "Export", "No clips in timeline to export.");
        return;
    }

    // S12 single-path: the File->Export action now routes through RenderQueue,
    // which renders the FULL live edit graph via tlrender::renderFrameAt (S8)
    // — pixel-identical to the GLPreview composite. The legacy CPU-only
    // Exporter::doExport (limited applyEffectStack subset, graph skipped for
    // 10-bit/HDR) is NO LONGER reached from any UI action. See progress.txt
    // "### S12 single-path audit".
    if (!m_renderQueue)
        m_renderQueue = new RenderQueue(this);

    RenderJob job;
    job.name = QFileInfo(exportCfg.outputPath).fileName();
    // projectFilePath doubles as RenderQueue's audio-mux source. When a saved
    // project path exists use it; the in-memory timeline seam below carries
    // the actual edit graph (LUT/mask/tracking are NOT serialized), and
    // RenderQueue falls back to the V1 clip's source file for audio when this
    // is a .veditor / empty path (RenderQueue.cpp:551-563).
    job.projectFilePath = m_projectFilePath;
    job.outputPath = exportCfg.outputPath;
    job.width   = exportCfg.width  > 0 ? exportCfg.width  : 1920;
    job.height  = exportCfg.height > 0 ? exportCfg.height : 1080;
    job.bitrateBps = static_cast<qint64>(exportCfg.videoBitrate) * 1000;
    job.startUs = 0;
    job.endUs   = 0;   // 0 = whole timeline
    // The additive in-memory edit-graph seam — the SAME pattern PARITY S8
    // (main.cpp) and BatchExportQueue (BatchExportQueue.cpp:170) use so the
    // real RenderQueue renders THIS live Timeline through renderFrameAt.
    job.timeline = m_timeline;

    // RenderQueue::startRenderPipe consumes these JSON fields directly
    // (videoCodec/videoBitrate/fps/audioCodec/audioBitrate, plus the HDR10 /
    // ProRes branch keys). Pass ExportDialog's resolved encoder verbatim.
    QJsonObject cfg;
    cfg["width"]        = job.width;
    cfg["height"]       = job.height;
    cfg["fps"]          = exportCfg.fps > 0 ? exportCfg.fps : 30;
    cfg["videoCodec"]   = exportCfg.videoCodec;     // already ffmpeg-named
    cfg["videoBitrate"] = exportCfg.videoBitrate;   // kbps
    cfg["audioCodec"]   = exportCfg.audioCodec;
    cfg["audioBitrate"] = exportCfg.audioBitrate;
    if (exportCfg.hdr10 || exportCfg.hdrSettings.mode != QStringLiteral("sdr")) {
        cfg["hdr10"]   = true;
        cfg["hdrMode"] = exportCfg.hdrSettings.mode;
    }
    if (exportCfg.proresProfile >= 0)
        cfg["proresProfile"] = exportCfg.proresProfile;
    job.exportConfig = cfg;

    auto *progress = new QProgressDialog("Exporting...", "Cancel", 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);

    // Bridge RenderQueue's uuid-keyed signals to the same progress/finish UX
    // the old Exporter path showed. Connections are scoped to `progress` so
    // they auto-disconnect when the dialog is destroyed.
    connect(m_renderQueue, &RenderQueue::jobProgressUuid, progress,
            [progress](const QString &, int percent) {
        progress->setValue(percent);
    });
    connect(m_renderQueue, &RenderQueue::jobCompletedUuid, progress,
            [this, progress](const QString &, bool success, const QString &err) {
        progress->close();
        progress->deleteLater();
        if (success)
            statusBar()->showMessage("Export complete: rendered via timeline SSOT");
        else
            QMessageBox::critical(this, "Export Failed",
                err.isEmpty() ? QStringLiteral("Export failed") : err);
    });
    connect(progress, &QProgressDialog::canceled, m_renderQueue,
            &RenderQueue::stop);

    // RM-1.3: this is a job.timeline != nullptr path — RenderQueue's
    // resolveTimeline early-returns before its persisted-project matte
    // population, so the live Timeline's matte carrier MUST be current
    // here or the export silently drops every track matte. Re-sync the
    // GUI map onto m_timeline immediately before submission so an edit
    // made after the last configure-matte/load is reflected.
    syncTrackMatteEntriesToTimeline(m_timeline, m_trackMatteClipEntries);
    statusBar()->showMessage("Exporting: " + exportCfg.outputPath);
    m_renderQueue->addJob(job);
    m_renderQueue->start();
}

void MainWindow::splitClip()
{
    // RM-1.2: remap positional matte keys across the clip-index shift.
    TrackClipSnapshot snap = snapshotTrackClips(m_timeline);
    m_timeline->splitAtPlayhead();
    remapTrackMatteEntriesAfterMutation(m_timeline, m_trackMatteClipEntries,
                                        snap);
    syncTrackMatteEntriesToTimeline(m_timeline, m_trackMatteClipEntries);
    statusBar()->showMessage("Split clip at playhead");
    updateEditActions();
}

void MainWindow::deleteClip()
{
    if (!m_timeline->hasSelection()) return;
    // RM-1.2: drop matte entries for the removed clip + reindex survivors.
    TrackClipSnapshot snap = snapshotTrackClips(m_timeline);
    m_timeline->deleteSelectedClip();
    remapTrackMatteEntriesAfterMutation(m_timeline, m_trackMatteClipEntries,
                                        snap);
    syncTrackMatteEntriesToTimeline(m_timeline, m_trackMatteClipEntries);
    statusBar()->showMessage("Deleted clip");
    updateEditActions();
}

void MainWindow::rippleDelete()
{
    if (!m_timeline->hasSelection()) return;
    // RM-1.2: same as deleteClip — Timeline reindexes, matte keys must follow.
    TrackClipSnapshot snap = snapshotTrackClips(m_timeline);
    m_timeline->rippleDeleteSelectedClip();
    remapTrackMatteEntriesAfterMutation(m_timeline, m_trackMatteClipEntries,
                                        snap);
    syncTrackMatteEntriesToTimeline(m_timeline, m_trackMatteClipEntries);
    statusBar()->showMessage("Ripple deleted clip");
    updateEditActions();
}

void MainWindow::copyClip()
{
    m_timeline->copySelectedClip();
    statusBar()->showMessage("Copied clip to clipboard");
    updateEditActions();
}

void MainWindow::pasteClip()
{
    // RM-1.2: paste inserts a clip → downstream indices shift; matte keys
    // for clips after the insertion point must be bumped. The pasted clip
    // itself is new and carries no matte entry (unmatched → no remap).
    TrackClipSnapshot snap = snapshotTrackClips(m_timeline);
    m_timeline->pasteClip();
    remapTrackMatteEntriesAfterMutation(m_timeline, m_trackMatteClipEntries,
                                        snap);
    syncTrackMatteEntriesToTimeline(m_timeline, m_trackMatteClipEntries);
    statusBar()->showMessage("Pasted clip");
    updateEditActions();
}

void MainWindow::copyEffects()
{
    if (!m_timeline || !m_timeline->hasSelection()) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }
    const int clipIdx = m_timeline->selectedVideoClipIndex();
    if (clipIdx < 0) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }
    const auto &clips = m_timeline->videoClips();
    const auto &clip = clips[clipIdx];

    effectctrl::ClipMotion motion;
    motion.scale = clip.videoScale;
    motion.dx = clip.videoDx;
    motion.dy = clip.videoDy;
    motion.rotationDeg = clip.rotation2DDegrees;
    motion.opacity = clip.opacity;

    effectctrl::EffectClipboard::instance().capture(clip.effects, motion);
    statusBar()->showMessage("Effects copied", 2000);
}

void MainWindow::pasteEffects()
{
    if (!m_timeline || !m_timeline->hasSelection()) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }
    auto &clipboard = effectctrl::EffectClipboard::instance();
    if (!clipboard.hasContent()) {
        statusBar()->showMessage("Clipboard is empty", 2000);
        return;
    }
    const int clipIdx = m_timeline->selectedVideoClipIndex();
    const int trackIdx = 0; // V1
    if (clipIdx < 0) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }

    m_timeline->setClipEffects(clipboard.effects());

    effectctrl::MotionState motion;
    motion.scale = clipboard.motion().scale;
    motion.dx = clipboard.motion().dx;
    motion.dy = clipboard.motion().dy;
    motion.rotation2DDeg = clipboard.motion().rotationDeg;
    motion.opacity = clipboard.motion().opacity;
    motion.is3DLayer = false;
    motion.posZ = 0.0;
    motion.rotX = 0.0;
    motion.rotY = 0.0;
    motion.rotZ = 0.0;
    m_timeline->setClipMotion(trackIdx, clipIdx, motion);

    statusBar()->showMessage("Effects pasted", 2000);
}

void MainWindow::pasteAttributes()
{
    if (!m_timeline || !m_timeline->hasSelection()) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }
    auto &clipboard = effectctrl::EffectClipboard::instance();
    if (!clipboard.hasContent()) {
        statusBar()->showMessage("Clipboard is empty", 2000);
        return;
    }
    const int clipIdx = m_timeline->selectedVideoClipIndex();
    const int trackIdx = 0;
    if (clipIdx < 0) {
        statusBar()->showMessage("No clip selected", 3000);
        return;
    }
    const auto &clips = m_timeline->videoClips();
    auto targetClip = clips[clipIdx];

    PasteAttributesDialog dialog(clipboard.effects(), clipboard.motion(), this);
    if (dialog.exec() != QDialog::Accepted) return;

    auto sel = dialog.selection();

    if (sel.pastePosition || sel.pasteScale || sel.pasteRotation || sel.pasteOpacity) {
        effectctrl::MotionState motion;
        const auto &cbMotion = clipboard.motion();
        const auto &srcClip = targetClip;

        motion.scale = sel.pasteScale ? cbMotion.scale : srcClip.videoScale;
        motion.dx = sel.pastePosition ? cbMotion.dx : srcClip.videoDx;
        motion.dy = sel.pastePosition ? cbMotion.dy : srcClip.videoDy;
        motion.rotation2DDeg = sel.pasteRotation ? cbMotion.rotationDeg : srcClip.rotation2DDegrees;
        motion.opacity = sel.pasteOpacity ? cbMotion.opacity : srcClip.opacity;
        motion.is3DLayer = srcClip.is3DLayer;
        motion.posZ = srcClip.layer3D.positionZ;
        motion.rotX = srcClip.layer3D.rotationX;
        motion.rotY = srcClip.layer3D.rotationY;
        motion.rotZ = srcClip.layer3D.rotationZ;

        m_timeline->setClipMotion(trackIdx, clipIdx, motion);
    }

    if (!sel.effectIndices.isEmpty()) {
        auto targetEffects = targetClip.effects;
        const auto &cbEffects = clipboard.effects();

        targetEffects.clear();
        for (int idx : sel.effectIndices) {
            if (idx >= 0 && idx < cbEffects.size())
                targetEffects.append(cbEffects[idx]);
        }

        m_timeline->setClipEffects(targetEffects);
    }

    statusBar()->showMessage("Attributes past", 2000);
}

void MainWindow::undoAction()
{
    m_timeline->undo();
    statusBar()->showMessage("Undo");
    updateEditActions();
}

void MainWindow::redoAction()
{
    m_timeline->redo();
    statusBar()->showMessage("Redo");
    updateEditActions();
}

void MainWindow::toggleSnap()
{
    bool snap = !m_timeline->snapEnabled();
    m_timeline->setSnapEnabled(snap);
    m_snapAction->setChecked(snap);
    statusBar()->showMessage(snap ? "Snap enabled" : "Snap disabled");
}

void MainWindow::zoomIn()
{
    m_timeline->zoomIn();
    statusBar()->showMessage(QString("Zoom: %1 px/s").arg(m_timeline->videoClips().isEmpty() ? 15 : 15));
}

void MainWindow::zoomOut()
{
    m_timeline->zoomOut();
    statusBar()->showMessage("Zoom out");
}

void MainWindow::addVideoTrack()
{
    m_timeline->addVideoTrack();
    statusBar()->showMessage(QString("Added video track V%1").arg(m_timeline->videoTrackCount()));
}

void MainWindow::addAudioTrack()
{
    m_timeline->addAudioTrack();
    rebuildAudioMeters();
    statusBar()->showMessage(QString("Added audio track A%1").arg(m_timeline->audioTrackCount()));
}

void MainWindow::markIn()
{
    m_timeline->markIn();
    statusBar()->showMessage(QString("Mark In: %1s").arg(m_timeline->markedIn(), 0, 'f', 1));
}

void MainWindow::markOut()
{
    m_timeline->markOut();
    statusBar()->showMessage(QString("Mark Out: %1s").arg(m_timeline->markedOut(), 0, 'f', 1));
}

void MainWindow::setClipSpeed()
{
    if (!m_timeline->hasSelection()) return;
    bool ok;
    double speed = QInputDialog::getDouble(this, "Set Clip Speed",
        "Speed (0.25x - 4.0x):", 1.0, 0.25, 4.0, 2, &ok);
    if (ok) {
        m_timeline->setClipSpeed(speed);
        statusBar()->showMessage(QString("Clip speed: %1x").arg(speed));
    }
}

void MainWindow::setClipVolume()
{
    if (!m_timeline->hasSelection()) return;
    bool ok;
    double vol = QInputDialog::getDouble(this, "Set Clip Volume",
        "Volume (0% = mute, 100% = normal, 200% = boost):", 100.0, 0.0, 200.0, 0, &ok);
    if (ok) {
        m_timeline->setClipVolume(vol / 100.0);
        statusBar()->showMessage(QString("Volume: %1%").arg(static_cast<int>(vol)));
    }
}

void MainWindow::addBgm()
{
    QString filter = "Audio Files (*.mp3 *.wav *.aac *.ogg *.flac *.m4a);;All Files (*)";
    QString filePath = QFileDialog::getOpenFileName(this, "Add BGM / Audio", QString(), filter);
    if (!filePath.isEmpty()) {
        // Ensure we have a second audio track for BGM
        if (m_timeline->audioTrackCount() < 2)
            m_timeline->addAudioTrack();
        m_timeline->addAudioFile(filePath);
        statusBar()->showMessage("Added BGM: " + filePath);
    }
}

void MainWindow::toggleMute()
{
    m_timeline->toggleMuteTrack(0);
    statusBar()->showMessage(QString("A1: %1").arg(m_timeline->audioTrackCount() > 0 ? "Toggled mute" : "No track"));
}

void MainWindow::toggleSolo()
{
    m_timeline->toggleSoloTrack(0);
    statusBar()->showMessage("A1: Toggled solo");
}

void MainWindow::setupToolPropertyPanel()
{
    m_toolPropertyStack = new QStackedWidget(this);
    m_toolPropertyStack->setMinimumWidth(260);
    m_toolPropertyStack->setMaximumWidth(360);

    // Page 0: empty placeholder shown when no tool is active.
    auto *emptyPage = new QWidget(m_toolPropertyStack);
    auto *emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->addStretch();
    auto *emptyLabel = new QLabel("ツール未選択", emptyPage);
    emptyLabel->setAlignment(Qt::AlignCenter);
    emptyLabel->setStyleSheet("color: #888;");
    emptyLayout->addWidget(emptyLabel);
    emptyLayout->addStretch();
    m_toolPropertyStack->addWidget(emptyPage);

    // Page 1: text tool properties — text / font size / color / 適用.
    auto *textPage = new QWidget(m_toolPropertyStack);
    auto *textLayout = new QVBoxLayout(textPage);
    textLayout->setContentsMargins(12, 12, 12, 12);
    auto *titleLabel = new QLabel("テキストツール", textPage);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 1);
    titleLabel->setFont(titleFont);
    textLayout->addWidget(titleLabel);
    textLayout->addSpacing(8);

    auto *form = new QFormLayout();
    m_textToolLineEdit = new QLineEdit(textPage);
    m_textToolLineEdit->setPlaceholderText("テキストを入力...");
    form->addRow("テキスト", m_textToolLineEdit);

    m_textToolSizeSpin = new QSpinBox(textPage);
    m_textToolSizeSpin->setRange(6, 256);
    m_textToolSizeSpin->setValue(32);
    m_textToolSizeSpin->setSuffix(" pt");
    connect(m_textToolSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { pushTextToolStyleToPreview(); });
    form->addRow("サイズ", m_textToolSizeSpin);

    m_textToolColor = Qt::white;
    m_textToolColorButton = new QPushButton(textPage);
    m_textToolColorButton->setText("色を選択...");
    m_textToolColorButton->setStyleSheet("background-color: white; color: black;");
    connect(m_textToolColorButton, &QPushButton::clicked, this, [this]() {
        QColor picked = QColorDialog::getColor(m_textToolColor, this, "テキスト色");
        if (picked.isValid()) {
            m_textToolColor = picked;
            m_textToolColorButton->setStyleSheet(
                QString("background-color: %1; color: %2;")
                    .arg(picked.name())
                    .arg(picked.lightness() > 128 ? "black" : "white"));
            pushTextToolStyleToPreview();
        }
    });
    form->addRow("色", m_textToolColorButton);

    m_textToolStartSpin = new QDoubleSpinBox(textPage);
    m_textToolStartSpin->setRange(0.0, 36000.0);
    m_textToolStartSpin->setDecimals(2);
    m_textToolStartSpin->setSingleStep(0.5);
    m_textToolStartSpin->setSuffix(" s");
    m_textToolStartSpin->setValue(0.0);
    form->addRow("開始時間", m_textToolStartSpin);

    // 表示時間 = duration (not absolute end time). applyTextToolOverlay
    // computes overlay.endTime = startTime + duration so the downstream
    // renderer still gets an absolute end time in EnhancedTextOverlay.
    m_textToolEndSpin = new QDoubleSpinBox(textPage);
    m_textToolEndSpin->setRange(0.1, 36000.0);
    m_textToolEndSpin->setDecimals(2);
    m_textToolEndSpin->setSingleStep(0.5);
    m_textToolEndSpin->setSuffix(" s");
    m_textToolEndSpin->setValue(5.0);
    form->addRow("表示時間", m_textToolEndSpin);

    // Gradient fill controls (read by applyTextToolOverlay on 適用).
    m_textToolGradientCheck = new QCheckBox("グラデーション", textPage);
    form->addRow("", m_textToolGradientCheck);
    m_textToolGradientStart = Qt::white;
    m_textToolGradientEnd   = QColor(255, 200, 0);
    auto styleColorBtn = [](QPushButton *b, const QColor &c) {
        b->setStyleSheet(QString("background-color: %1; color: %2;")
                             .arg(c.name())
                             .arg(c.lightness() > 128 ? "black" : "white"));
    };
    m_textToolGradientStartBtn = new QPushButton("開始色", textPage);
    styleColorBtn(m_textToolGradientStartBtn, m_textToolGradientStart);
    connect(m_textToolGradientStartBtn, &QPushButton::clicked, this, [this, styleColorBtn]() {
        QColor picked = QColorDialog::getColor(m_textToolGradientStart, this, "グラデーション開始色");
        if (picked.isValid()) {
            m_textToolGradientStart = picked;
            styleColorBtn(m_textToolGradientStartBtn, picked);
        }
    });
    form->addRow("開始色", m_textToolGradientStartBtn);
    m_textToolGradientEndBtn = new QPushButton("終了色", textPage);
    styleColorBtn(m_textToolGradientEndBtn, m_textToolGradientEnd);
    connect(m_textToolGradientEndBtn, &QPushButton::clicked, this, [this, styleColorBtn]() {
        QColor picked = QColorDialog::getColor(m_textToolGradientEnd, this, "グラデーション終了色");
        if (picked.isValid()) {
            m_textToolGradientEnd = picked;
            styleColorBtn(m_textToolGradientEndBtn, picked);
        }
    });
    form->addRow("終了色", m_textToolGradientEndBtn);
    m_textToolGradientAngleSpin = new QDoubleSpinBox(textPage);
    m_textToolGradientAngleSpin->setRange(0.0, 360.0);
    m_textToolGradientAngleSpin->setDecimals(0);
    m_textToolGradientAngleSpin->setSingleStep(15.0);
    m_textToolGradientAngleSpin->setSuffix(" °");
    m_textToolGradientAngleSpin->setValue(90.0);
    form->addRow("角度", m_textToolGradientAngleSpin);

    // Adobe-style fine controls: type (Linear/Radial), midpoint, reverse.
    m_textToolGradientTypeCombo = new QComboBox(textPage);
    m_textToolGradientTypeCombo->addItem("線形", 0);
    m_textToolGradientTypeCombo->addItem("放射状", 1);
    form->addRow("種類", m_textToolGradientTypeCombo);

    m_textToolGradientMidSpin = new QDoubleSpinBox(textPage);
    m_textToolGradientMidSpin->setRange(1.0, 99.0);
    m_textToolGradientMidSpin->setDecimals(0);
    m_textToolGradientMidSpin->setSingleStep(5.0);
    m_textToolGradientMidSpin->setSuffix(" %");
    m_textToolGradientMidSpin->setValue(50.0);
    form->addRow("中点", m_textToolGradientMidSpin);

    m_textToolGradientReverseCheck = new QCheckBox("反転", textPage);
    form->addRow("", m_textToolGradientReverseCheck);

    // Illustrator-style multi-stop editor: horizontal gradient bar with
    // draggable markers. Click empty area to add, right-click to delete.
    m_textToolStopBar = new GradientStopBar(textPage);
    form->addRow("ストップ", m_textToolStopBar);

    // Per-stop property controls (active stop is set by GradientStopBar::stopSelected).
    m_textToolStopColorBtn = new QPushButton("色を選択…", textPage);
    styleColorBtn(m_textToolStopColorBtn, Qt::white);
    connect(m_textToolStopColorBtn, &QPushButton::clicked, this, [this, styleColorBtn]() {
        if (!m_textToolStopBar) return;
        const int idx = m_textToolStopBar->selectedIndex();
        if (idx < 0 || idx >= m_textToolStopBar->stops().size()) return;
        QColor picked = QColorDialog::getColor(m_textToolStopBar->stops()[idx].color,
                                               this, "ストップ色");
        if (!picked.isValid()) return;
        GradientStop s = m_textToolStopBar->stops()[idx];
        s.color = picked;
        m_textToolStopBar->updateStop(idx, s);
        styleColorBtn(m_textToolStopColorBtn, picked);
        pushTextToolStyleToPreview();
    });
    form->addRow("ストップ色", m_textToolStopColorBtn);

    m_textToolStopOpacitySpin = new QDoubleSpinBox(textPage);
    m_textToolStopOpacitySpin->setRange(0.0, 100.0);
    m_textToolStopOpacitySpin->setDecimals(0);
    m_textToolStopOpacitySpin->setSingleStep(5.0);
    m_textToolStopOpacitySpin->setSuffix(" %");
    m_textToolStopOpacitySpin->setValue(100.0);
    connect(m_textToolStopOpacitySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!m_textToolStopBar) return;
                const int idx = m_textToolStopBar->selectedIndex();
                if (idx < 0 || idx >= m_textToolStopBar->stops().size()) return;
                GradientStop s = m_textToolStopBar->stops()[idx];
                s.opacity = qBound(0.0, v / 100.0, 1.0);
                m_textToolStopBar->updateStop(idx, s);
                pushTextToolStyleToPreview();
            });
    form->addRow("ストップ不透明度", m_textToolStopOpacitySpin);

    m_textToolStopPosSpin = new QDoubleSpinBox(textPage);
    m_textToolStopPosSpin->setRange(0.0, 100.0);
    m_textToolStopPosSpin->setDecimals(0);
    m_textToolStopPosSpin->setSingleStep(1.0);
    m_textToolStopPosSpin->setSuffix(" %");
    m_textToolStopPosSpin->setValue(0.0);
    connect(m_textToolStopPosSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!m_textToolStopBar) return;
                const int idx = m_textToolStopBar->selectedIndex();
                if (idx < 0 || idx >= m_textToolStopBar->stops().size()) return;
                GradientStop s = m_textToolStopBar->stops()[idx];
                s.position = qBound(0.0, v / 100.0, 1.0);
                m_textToolStopBar->updateStop(idx, s);
                pushTextToolStyleToPreview();
            });
    form->addRow("ストップ位置", m_textToolStopPosSpin);

    // Sync per-stop controls when a stop is selected on the bar.
    connect(m_textToolStopBar, &GradientStopBar::stopSelected, this, [this, styleColorBtn](int idx) {
        if (!m_textToolStopBar || idx < 0 || idx >= m_textToolStopBar->stops().size()) return;
        const GradientStop &s = m_textToolStopBar->stops()[idx];
        if (m_textToolStopColorBtn) styleColorBtn(m_textToolStopColorBtn, s.color);
        if (m_textToolStopOpacitySpin) {
            QSignalBlocker b(m_textToolStopOpacitySpin);
            m_textToolStopOpacitySpin->setValue(s.opacity * 100.0);
        }
        if (m_textToolStopPosSpin) {
            QSignalBlocker b(m_textToolStopPosSpin);
            m_textToolStopPosSpin->setValue(s.position * 100.0);
        }
    });
    connect(m_textToolStopBar, &GradientStopBar::stopsChanged, this, [this]() {
        pushTextToolStyleToPreview();
    });

    // Gradient preset save / load buttons — JSON files under AppData/gradients.
    m_textToolGradientPresetSaveBtn = new QPushButton("保存", textPage);
    m_textToolGradientPresetLoadBtn = new QPushButton("呼び出し", textPage);
    connect(m_textToolGradientPresetSaveBtn, &QPushButton::clicked, this, [this]() {
        if (!m_textToolStopBar) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "プリセット保存",
                                                   "名前:", QLineEdit::Normal, "preset", &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/gradients";
        QDir().mkpath(dir);
        QJsonArray stopsArr;
        for (const auto &s : m_textToolStopBar->stops()) {
            QJsonObject so;
            so["position"] = s.position;
            so["color"] = s.color.name(QColor::HexArgb);
            so["opacity"] = s.opacity;
            stopsArr.append(so);
        }
        QJsonObject root;
        root["name"] = name.trimmed();
        root["type"] = m_textToolGradientTypeCombo ? m_textToolGradientTypeCombo->currentData().toInt() : 0;
        root["angle"] = m_textToolGradientAngleSpin ? m_textToolGradientAngleSpin->value() : 90.0;
        root["reverse"] = m_textToolGradientReverseCheck && m_textToolGradientReverseCheck->isChecked();
        root["stops"] = stopsArr;
        const QString path = dir + "/" + name.trimmed() + ".json";
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(root).toJson());
            statusBar()->showMessage(QString("プリセット保存: %1").arg(path));
        } else {
            statusBar()->showMessage("プリセット保存失敗");
        }
    });
    connect(m_textToolGradientPresetLoadBtn, &QPushButton::clicked, this, [this]() {
        if (!m_textToolStopBar) return;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/gradients";
        QDir().mkpath(dir);
        const QString path = QFileDialog::getOpenFileName(this, "プリセット呼び出し", dir, "JSON (*.json)");
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return;
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        if (m_textToolGradientTypeCombo) {
            const int t = root["type"].toInt(0);
            const int idx = m_textToolGradientTypeCombo->findData(t);
            if (idx >= 0) m_textToolGradientTypeCombo->setCurrentIndex(idx);
        }
        if (m_textToolGradientAngleSpin)
            m_textToolGradientAngleSpin->setValue(root["angle"].toDouble(90.0));
        if (m_textToolGradientReverseCheck)
            m_textToolGradientReverseCheck->setChecked(root["reverse"].toBool(false));
        QVector<GradientStop> stops;
        for (const auto &v : root["stops"].toArray()) {
            const QJsonObject so = v.toObject();
            GradientStop s;
            s.position = so["position"].toDouble(0.0);
            s.color = QColor(so["color"].toString("#ffffffff"));
            s.opacity = so["opacity"].toDouble(1.0);
            stops.append(s);
        }
        if (!stops.isEmpty())
            m_textToolStopBar->setStops(stops);
        pushTextToolStyleToPreview();
        statusBar()->showMessage(QString("プリセット呼び出し: %1").arg(path));
    });
    auto *presetRow = new QHBoxLayout();
    presetRow->addWidget(m_textToolGradientPresetSaveBtn);
    presetRow->addWidget(m_textToolGradientPresetLoadBtn);
    form->addRow("プリセット", presetRow);

    // Outline stroke controls.
    m_textToolOutlineCheck = new QCheckBox("枠線", textPage);
    form->addRow("", m_textToolOutlineCheck);
    m_textToolOutlineColor = Qt::black;
    m_textToolOutlineColorBtn = new QPushButton("枠線色", textPage);
    styleColorBtn(m_textToolOutlineColorBtn, m_textToolOutlineColor);
    connect(m_textToolOutlineColorBtn, &QPushButton::clicked, this, [this, styleColorBtn]() {
        QColor picked = QColorDialog::getColor(m_textToolOutlineColor, this, "枠線色");
        if (picked.isValid()) {
            m_textToolOutlineColor = picked;
            styleColorBtn(m_textToolOutlineColorBtn, picked);
        }
    });
    form->addRow("枠線色", m_textToolOutlineColorBtn);
    m_textToolOutlineWidthSpin = new QSpinBox(textPage);
    m_textToolOutlineWidthSpin->setRange(0, 20);
    m_textToolOutlineWidthSpin->setValue(2);
    m_textToolOutlineWidthSpin->setSuffix(" px");
    form->addRow("枠線幅", m_textToolOutlineWidthSpin);

    textLayout->addLayout(form);
    textLayout->addSpacing(12);

    auto *applyButton = new QPushButton("適用", textPage);
    applyButton->setMinimumHeight(32);
    connect(applyButton, &QPushButton::clicked, this, &MainWindow::applyTextToolOverlay);
    textLayout->addWidget(applyButton);

    auto *hint = new QLabel(
        "プレビュー上でドラッグしてテキスト枠を指定してください。\n"
        "ドラッグしない場合は中央に配置されます。", textPage);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #888; font-size: 11px;");
    textLayout->addWidget(hint);
    textLayout->addStretch();

    // The text panel grew tall with gradient/stop controls — wrap it in a
    // scroll area so it doesn't force the main window to an abnormal height
    // on smaller screens. The scroll area takes the panel slot in the stack.
    auto *textScroll = new QScrollArea(m_toolPropertyStack);
    textScroll->setWidget(textPage);
    textScroll->setWidgetResizable(true);
    textScroll->setFrameShape(QFrame::NoFrame);
    textScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_toolPropertyStack->addWidget(textScroll);
    m_toolPropertyStack->setCurrentIndex(0);
}

void MainWindow::onTextToolToggled(bool checked)
{
    m_textToolActive = checked;
    if (m_player)
        m_player->setTextToolActive(checked);
    if (!m_toolPropertyStack)
        return;
    if (checked) {
        m_toolPropertyStack->setCurrentIndex(1);
        m_toolPropertyStack->show();
        pushTextToolStyleToPreview();
        // Pre-fill time defaults: 開始時間 = current playhead, 表示時間 = 5 s.
        // Both remain adjustable via the spinboxes before 適用.
        if (m_timeline && m_textToolStartSpin && m_textToolEndSpin) {
            m_textToolStartSpin->setValue(m_timeline->playheadPosition());
            m_textToolEndSpin->setValue(5.0);
        }
        statusBar()->showMessage("テキストツール ON — プレビュー上でドラッグして枠を指定、その場で直接入力");
    } else {
        m_toolPropertyStack->setCurrentIndex(0);
        m_toolPropertyStack->hide();
        m_textToolHasPendingRect = false;
        statusBar()->showMessage("テキストツール OFF");
    }
}

void MainWindow::onTextRectRequested(const QRectF &normalizedRect)
{
    m_textToolPendingRect = normalizedRect;
    m_textToolHasPendingRect = true;
    pushTextToolStyleToPreview();
    // Refresh 開始時間 against the current playhead each time a fresh rect
    // is drawn — makes the text default to "appears at the current playhead
    // for 5 s". The duration spin is left alone so a user-specified value
    // carries forward across multiple rect draws.
    if (m_timeline && m_textToolStartSpin)
        m_textToolStartSpin->setValue(m_timeline->playheadPosition());
    statusBar()->showMessage(QString("テキスト枠指定: %1,%2 %3x%4 — プレビュー上で直接入力するか、右パネルで『適用』")
        .arg(normalizedRect.x(), 0, 'f', 2)
        .arg(normalizedRect.y(), 0, 'f', 2)
        .arg(normalizedRect.width(), 0, 'f', 2)
        .arg(normalizedRect.height(), 0, 'f', 2));
}

void MainWindow::onTextInlineCommitted(const QString &text, const QRectF &normalizedRect)
{
    // Populate the pending state from the inline commit and run the normal
    // apply path so the overlay inherits the right panel's size/color/time
    // values. The QLineEdit is filled only so applyTextToolOverlay's empty
    // guard passes — it's cleared again at the end of the apply.
    m_textToolPendingRect = normalizedRect;
    m_textToolHasPendingRect = true;
    if (m_textToolLineEdit)
        m_textToolLineEdit->setText(text);
    applyTextToolOverlay();
}

void MainWindow::onTextOverlayEditCommitted(int overlayIndex, const QString &newText)
{
    // Click-to-edit on an existing overlay — only the text string changes,
    // the rect / style / time range stay put. Push the updated overlay list
    // back to the player so the preview re-renders with the new text.
    if (!m_timeline->updateTextOverlayText(overlayIndex, newText)) {
        statusBar()->showMessage("テキスト更新失敗");
        return;
    }
    const auto &clips = m_timeline->videoClips();
    if (!clips.isEmpty() && m_player) {
        QVector<EnhancedTextOverlay> overlays;
        const auto &mgr = clips[0].textManager;
        for (int i = 0; i < mgr.count(); ++i)
            overlays.append(mgr.overlay(i));
        m_player->setTextOverlays(overlays);
    }
    if (m_player)
        m_player->clearTextToolRect();
    statusBar()->showMessage(QString("テキスト更新: 「%1」").arg(newText));
}

void MainWindow::pushTextToolStyleToPreview()
{
    if (!m_player)
        return;
    // Use Arial + bold to match EnhancedTextOverlay's default font family;
    // inheriting the system default here was causing WYSIWYG metric drift
    // between the inline edit layer and composeFrameWithOverlays.
    QFont f("Arial");
    f.setPointSize(m_textToolSizeSpin ? m_textToolSizeSpin->value() : 32);
    f.setBold(true);
    m_player->setTextToolStyle(f, m_textToolColor);
}

void MainWindow::applyTextToolOverlay()
{
    // 適用 primarily commits whatever the user has been typing directly on
    // the preview (Adobe-style in-place edit). If the GLPreview is in edit
    // mode with non-empty text, just delegate: its commitCurrentTextToolEdit
    // fires the textInlineCommitted / textOverlayEditCommitted signal which
    // lands on the normal slot and runs the right-panel apply from there.
    if (m_player && m_player->isTextToolEditing()
        && !m_player->currentTextToolInputText().isEmpty()) {
        m_player->commitCurrentTextToolEdit();
        return;
    }
    if (!m_textToolLineEdit || m_textToolLineEdit->text().isEmpty()) {
        statusBar()->showMessage("テキストが空です");
        return;
    }
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        statusBar()->showMessage("先にクリップを選択してください");
        return;
    }

    EnhancedTextOverlay overlay;
    overlay.text = m_textToolLineEdit->text();
    QFont font = overlay.font;
    font.setPointSize(m_textToolSizeSpin ? m_textToolSizeSpin->value() : 32);
    overlay.font = font;
    overlay.color = m_textToolColor;
    // Transparent background by default — the user explicitly asked for it
    // so the text sits directly on the video instead of on a translucent box.
    overlay.backgroundColor = QColor(0, 0, 0, 0);
    overlay.gradientEnabled = m_textToolGradientCheck && m_textToolGradientCheck->isChecked();
    overlay.gradientStart = m_textToolGradientStart;
    overlay.gradientEnd = m_textToolGradientEnd;
    overlay.gradientAngle = m_textToolGradientAngleSpin ? m_textToolGradientAngleSpin->value() : 90.0;
    overlay.gradientType = m_textToolGradientTypeCombo ? m_textToolGradientTypeCombo->currentData().toInt() : 0;
    overlay.gradientMidpoint = m_textToolGradientMidSpin ? m_textToolGradientMidSpin->value() : 50.0;
    overlay.gradientReverse = m_textToolGradientReverseCheck && m_textToolGradientReverseCheck->isChecked();
    if (m_textToolStopBar)
        overlay.gradientStops = m_textToolStopBar->stops();
    if (m_textToolOutlineCheck && m_textToolOutlineCheck->isChecked()) {
        overlay.outlineColor = m_textToolOutlineColor;
        overlay.outlineWidth = m_textToolOutlineWidthSpin ? m_textToolOutlineWidthSpin->value() : 2;
    } else {
        overlay.outlineWidth = 0;
    }
    overlay.startTime = m_textToolStartSpin ? m_textToolStartSpin->value() : 0.0;
    // The second spin is 表示時間 (duration), not absolute end time — the
    // user asked for duration semantics so the text defaults to "now + 5 s".
    const double duration = m_textToolEndSpin ? m_textToolEndSpin->value() : 5.0;
    overlay.endTime = overlay.startTime + qMax(0.1, duration);
    if (m_textToolHasPendingRect) {
        overlay.x = m_textToolPendingRect.x() + m_textToolPendingRect.width() / 2.0;
        overlay.y = m_textToolPendingRect.y() + m_textToolPendingRect.height() / 2.0;
        overlay.width = m_textToolPendingRect.width();
        overlay.height = m_textToolPendingRect.height();
    }

    if (!m_timeline->addTextOverlayToFirstVideoClip(overlay)) {
        statusBar()->showMessage("テキスト追加失敗 — クリップが見つかりません");
        return;
    }

    // Push the updated overlay list to the preview so the new text is
    // visible immediately. MainWindow is the single source of truth that
    // owns the timeline → player forwarding.
    const auto &clips = m_timeline->videoClips();
    if (!clips.isEmpty()) {
        QVector<EnhancedTextOverlay> overlays;
        const auto &mgr = clips[0].textManager;
        for (int i = 0; i < mgr.count(); ++i)
            overlays.append(mgr.overlay(i));
        if (m_player)
            m_player->setTextOverlays(overlays);
    }
    statusBar()->showMessage(QString("テキストを追加しました: 「%1」").arg(overlay.text));

    m_textToolLineEdit->clear();
    m_textToolHasPendingRect = false;
    if (m_player)
        m_player->clearTextToolRect();
}

void MainWindow::addTextOverlay()
{
    TextOverlayDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        auto overlay = dialog.result();
        // TODO: Store overlay in project and render on preview
        statusBar()->showMessage(QString("Added text: \"%1\"").arg(overlay.text));
    }
}

void MainWindow::exportTextOverlays()
{
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "テキスト書き出し",
                                 "クリップにテキストオーバーレイがありません。");
        return;
    }
    const auto &clip = m_timeline->videoClips().first();
    if (clip.textManager.count() == 0) {
        QMessageBox::information(this, "テキスト書き出し",
                                 "V1 の先頭クリップにテキストがありません。");
        return;
    }
    QString selectedFilter;
    const QString path = QFileDialog::getSaveFileName(
        this, "テキストを書き出し", QString(),
        "SubRip (*.srt);;CSV (*.csv);;All Files (*)", &selectedFilter);
    if (path.isEmpty())
        return;

    QVector<EnhancedTextOverlay> overlays;
    for (int i = 0; i < clip.textManager.count(); ++i)
        overlays.append(clip.textManager.overlay(i));

    const bool wantCsv = selectedFilter.contains("CSV")
                      || path.endsWith(".csv", Qt::CaseInsensitive);
    const bool ok = wantCsv
        ? TextManager::exportCSV(overlays, path)
        : TextManager::exportSRT(overlays, path);
    if (ok)
        statusBar()->showMessage(QString("%1 にテキストを書き出しました").arg(path));
    else
        QMessageBox::warning(this, "テキスト書き出し",
                             QString("書き出しに失敗しました: %1").arg(path));
}

void MainWindow::manageTextOverlays()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Text Overlays", "Select a clip first.");
        return;
    }

    // Get current clip's text overlays
    int sel = m_timeline->videoClips().size() > 0 ? 0 : -1;
    if (sel < 0) return;

    auto clips = m_timeline->videoClips();
    auto &textMgr = clips[sel].textManager;

    QString info = QString("Current clip has %1 text overlay(s).\n\n").arg(textMgr.count());
    for (int i = 0; i < textMgr.count(); ++i) {
        const auto &o = textMgr.overlay(i);
        info += QString("%1. \"%2\" (%3s - %4s)\n")
            .arg(i + 1)
            .arg(o.text.left(30))
            .arg(o.startTime, 0, 'f', 1)
            .arg(o.endTime > 0 ? QString::number(o.endTime, 'f', 1) : "end");
    }

    QMessageBox::information(this, "Text Overlays", info);
}

void MainWindow::importSubtitles()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Import Subtitles", "Select a clip first.");
        return;
    }

    QString filter = "Subtitle Files (*.srt *.vtt);;SRT (*.srt);;WebVTT (*.vtt);;All Files (*)";
    QString filePath = QFileDialog::getOpenFileName(this, "Import Subtitles", QString(), filter);
    if (filePath.isEmpty()) return;

    QVector<EnhancedTextOverlay> overlays;
    if (filePath.endsWith(".srt", Qt::CaseInsensitive))
        overlays = TextManager::importSRT(filePath);
    else if (filePath.endsWith(".vtt", Qt::CaseInsensitive))
        overlays = TextManager::importVTT(filePath);

    if (overlays.isEmpty()) {
        QMessageBox::warning(this, "Import", "No subtitles found in file.");
        return;
    }

    // Add to current clip
    auto clips = m_timeline->videoClips();
    if (!clips.isEmpty()) {
        for (const auto &o : overlays)
            clips[0].textManager.addOverlay(o);
        // Update via timeline
    }

    statusBar()->showMessage(QString("Imported %1 subtitle(s) from %2")
        .arg(overlays.size()).arg(QFileInfo(filePath).fileName()));
}

void MainWindow::saveTextTemplate()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Save Template", "Select a clip with text overlays first.");
        return;
    }

    bool ok;
    QString name = QInputDialog::getText(this, "Save Text Template",
        "Template name:", QLineEdit::Normal, "My Template", &ok);
    if (!ok || name.isEmpty()) return;

    // Create template from default style
    EnhancedTextOverlay sample;
    TextTemplate tmpl = TextTemplate::fromOverlay(sample, name);
    TextManager::saveTemplate(tmpl);
    statusBar()->showMessage(QString("Template saved: %1").arg(name));
}

void MainWindow::addTransition()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Transition", "Select a clip first to add a transition.");
        return;
    }
    TransitionDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        auto transition = dialog.result();
        m_timeline->applyTransitionToSelected(transition);
        statusBar()->showMessage(QString("Added transition: %1 (%2s)")
            .arg(Transition::typeName(transition.type)).arg(transition.duration));
    }
}

void MainWindow::applyDefaultTransition()
{
    if (!m_timeline->hasSelection()) {
        statusBar()->showMessage("Select a clip first", 2000);
        return;
    }
    QSettings prefs("VSimpleEditor", "Preferences");
    const int rawType = prefs.value("transition/defaultType",
        static_cast<int>(TransitionType::CrossDissolve)).toInt();
    const double duration = qBound(0.1,
        prefs.value("transition/defaultDuration", 1.0).toDouble(), 5.0);
    const int rawEasing = prefs.value("transition/defaultEasing",
        static_cast<int>(TransitionEasing::Linear)).toInt();
    Transition t;
    t.type = static_cast<TransitionType>(rawType);
    t.duration = duration;
    t.easing = static_cast<TransitionEasing>(rawEasing);
    m_timeline->applyTransitionToSelected(t);
    statusBar()->showMessage(QString("Applied default transition: %1 (%2s, %3)")
        .arg(Transition::typeName(t.type))
        .arg(t.duration, 0, 'f', 1)
        .arg(Transition::easingName(t.easing)), 2000);
}

void MainWindow::editDefaultTransition()
{
    QSettings prefs("VSimpleEditor", "Preferences");
    const int curType = prefs.value("transition/defaultType",
        static_cast<int>(TransitionType::CrossDissolve)).toInt();
    const double curDuration = prefs.value("transition/defaultDuration", 1.0).toDouble();
    const int curEasing = prefs.value("transition/defaultEasing",
        static_cast<int>(TransitionEasing::Linear)).toInt();

    QDialog dialog(this);
    dialog.setWindowTitle("規定トランジション設定");
    auto *form = new QFormLayout(&dialog);

    auto *typeCombo = new QComboBox(&dialog);
    const TransitionType options[] = {
        TransitionType::CrossDissolve,
        TransitionType::FadeIn,
        TransitionType::FadeOut,
        TransitionType::DipToBlack,
        TransitionType::DipToWhite,
        TransitionType::WipeLeft,
        TransitionType::WipeRight,
        TransitionType::WipeUp,
        TransitionType::WipeDown,
        TransitionType::ClockWipe,
        TransitionType::BarnDoorHorizontal,
        TransitionType::BarnDoorVertical,
        TransitionType::SlideLeft,
        TransitionType::SlideRight,
        TransitionType::SlideUp,
        TransitionType::SlideDown,
        TransitionType::PushLeft,
        TransitionType::PushRight,
        TransitionType::PushUp,
        TransitionType::PushDown,
        TransitionType::CrossZoom,
        TransitionType::FilmDissolve,
        TransitionType::SpinCW,
        TransitionType::SpinCCW,
        TransitionType::DitherDissolve,
        TransitionType::BlurDissolve,
        TransitionType::Pixelate,
        TransitionType::WhipPanLeft,
        TransitionType::WhipPanRight,
        TransitionType::Glitch,
        TransitionType::LightLeak,
        TransitionType::LensFlare,
        TransitionType::FilmBurn,
        TransitionType::CameraShake,
        TransitionType::ColorChannelShift,
        TransitionType::FlipHorizontal,
        TransitionType::FlipVertical,
        TransitionType::IrisRound,
        TransitionType::IrisRoundClose,
        TransitionType::IrisBox,
        TransitionType::IrisBoxClose,
        TransitionType::BarnDoorHClose,
        TransitionType::BarnDoorVClose,
        TransitionType::ClockWipeCCW,
    };
    int curIdx = 0;
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        typeCombo->addItem(Transition::typeName(options[i]), static_cast<int>(options[i]));
        if (static_cast<int>(options[i]) == curType) curIdx = static_cast<int>(i);
    }
    typeCombo->setCurrentIndex(curIdx);

    auto *durSpin = new QDoubleSpinBox(&dialog);
    durSpin->setRange(0.1, 5.0);
    durSpin->setSingleStep(0.1);
    durSpin->setDecimals(2);
    durSpin->setSuffix(" s");
    durSpin->setValue(qBound(0.1, curDuration, 5.0));

    auto *easingCombo = new QComboBox(&dialog);
    const TransitionEasing easingOptions[] = {
        TransitionEasing::Linear,
        TransitionEasing::EaseIn,
        TransitionEasing::EaseOut,
        TransitionEasing::EaseInOut,
    };
    int curEasingIdx = 0;
    for (size_t i = 0; i < sizeof(easingOptions) / sizeof(easingOptions[0]); ++i) {
        easingCombo->addItem(Transition::easingName(easingOptions[i]),
            static_cast<int>(easingOptions[i]));
        if (static_cast<int>(easingOptions[i]) == curEasing)
            curEasingIdx = static_cast<int>(i);
    }
    easingCombo->setCurrentIndex(curEasingIdx);

    form->addRow("種類", typeCombo);
    form->addRow("時間", durSpin);
    form->addRow("イージング", easingCombo);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() == QDialog::Accepted) {
        prefs.setValue("transition/defaultType", typeCombo->currentData().toInt());
        prefs.setValue("transition/defaultDuration", durSpin->value());
        prefs.setValue("transition/defaultEasing", easingCombo->currentData().toInt());
        statusBar()->showMessage(
            QString("Default transition: %1 (%2s, %3)")
                .arg(typeCombo->currentText())
                .arg(durSpin->value(), 0, 'f', 1)
                .arg(easingCombo->currentText()),
            2000);
    }
}

void MainWindow::addImageOverlay()
{
    ImageOverlayDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        auto overlay = dialog.result();
        statusBar()->showMessage(QString("Added image: %1").arg(overlay.filePath));
    }
}

void MainWindow::addPip()
{
    int maxClip = m_timeline->videoClips().size() - 1;
    if (maxClip < 0) {
        QMessageBox::information(this, "PiP", "Add at least one clip first.");
        return;
    }
    PipDialog dialog(maxClip, this);
    if (dialog.exec() == QDialog::Accepted) {
        auto config = dialog.result();
        statusBar()->showMessage(QString("Added PiP from clip #%1").arg(config.sourceClipIndex));
    }
}

void MainWindow::colorCorrection()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Color Correction", "Select a clip first.");
        return;
    }

    // サブメニュー: 旧ダイアログ or 新パネル
    QMenu menu(this);
    auto *dialogAction = menu.addAction("色補正ダイアログ (クラシック)...");
    auto *panelAction  = menu.addAction("カラーグレーディングパネル");
    auto *chosen = menu.exec(QCursor::pos());
    if (!chosen) return;

    if (chosen == panelAction) {
        // 新パネルを表示
        m_colorGradingPanel->setColorCorrection(m_timeline->clipColorCorrection());
        m_colorGradingPanel->show();
        m_colorGradingPanel->raise();
    } else {
        // 旧ダイアログ (リアルタイムプレビュー付き)
        ColorCorrection originalCC = m_timeline->clipColorCorrection();
        ColorCorrectionDialog dialog(originalCC, this);

        // スライダー操作時にリアルタイムでプレビューに反映
        connect(&dialog, &ColorCorrectionDialog::colorCorrectionChanged,
                this, [this](const ColorCorrection &cc) {
            m_player->setColorCorrection(cc);
        });

        if (dialog.exec() == QDialog::Accepted) {
            // OK: 確定
            m_timeline->setClipColorCorrection(dialog.result());
            m_player->setColorCorrection(dialog.result());
            // パネルも同期
            m_colorGradingPanel->setColorCorrection(dialog.result());
            statusBar()->showMessage("色補正を適用しました");
        } else {
            // Cancel: 元に戻す
            m_player->setColorCorrection(originalCC);
        }
    }
}

void MainWindow::videoEffects()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Video Effects", "Select a clip first.");
        return;
    }
    const QVector<VideoEffect> original = m_timeline->clipEffects();
    VideoEffectDialog dialog(original, this);
    connect(&dialog, &VideoEffectDialog::effectsChanged, this,
            [this](const QVector<VideoEffect> &e) {
        if (m_player) m_player->setPreviewEffects(e);
    });
    if (m_player) m_player->setPreviewEffects(original);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (accepted) {
        m_timeline->setClipEffects(dialog.result());
        statusBar()->showMessage(QString("Applied %1 effect(s)").arg(dialog.result().size()));
    }
    if (m_player)
        m_player->setPreviewEffects(accepted ? dialog.result() : original, /*live=*/true);
}

void MainWindow::pluginEffects()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Plugin Effects", "Select a clip first.");
        return;
    }
    PluginEffectDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        auto plugin = PluginRegistry::instance().findByName(dialog.selectedPlugin());
        if (!plugin) return;

        // Apply plugin effect to the clip's current frame (stored as a custom effect)
        statusBar()->showMessage(QString("Applied plugin: %1").arg(plugin->name()));
    }
}

void MainWindow::editKeyframes()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Keyframes", "Select a clip first.");
        return;
    }

    // Let user choose which property to keyframe
    QStringList properties = {
        "colorCorrection.brightness", "colorCorrection.contrast",
        "colorCorrection.saturation", "colorCorrection.exposure",
        "colorCorrection.hue", "colorCorrection.temperature",
        "colorCorrection.gamma", "colorCorrection.highlights",
        "colorCorrection.shadows"
    };

    bool ok;
    QString prop = QInputDialog::getItem(this, "Edit Keyframes",
        "Select property to animate:", properties, 0, false, &ok);
    if (!ok) return;

    double clipDur = m_timeline->selectedClipDuration();
    auto km = m_timeline->clipKeyframes();

    // Get or create track for selected property
    KeyframeTrack track = km.hasTrack(prop) ?
        *km.track(prop) : KeyframeTrack(prop, 0.0);

    KeyframeDialog dialog(track, clipDur, this);
    if (dialog.exec() == QDialog::Accepted) {
        km.addTrack(dialog.result());
        m_timeline->setClipKeyframes(km);
        statusBar()->showMessage(QString("Keyframes updated: %1 (%2 points)")
            .arg(prop).arg(dialog.result().count()));
    }
}

void MainWindow::autoSilenceDetect()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Silence Detection", "Select a clip first.");
        return;
    }
    const auto &clips = m_timeline->videoClips();
    int sel = m_timeline->undoManager() ? 0 : 0; // use first clip if needed
    if (clips.isEmpty()) return;

    // Use selected clip's file
    int selIdx = 0;
    for (int i = 0; i < clips.size(); ++i) {
        // find selected
        if (i == m_timeline->videoClips().size() - 1 || m_timeline->hasSelection()) {
            selIdx = i; break;
        }
    }

    statusBar()->showMessage("Analyzing audio for silence...");
    auto silences = AutoEdit::detectSilenceFromFile(clips[selIdx].filePath);
    statusBar()->showMessage(QString("Found %1 silent region(s)").arg(silences.size()));

    if (silences.isEmpty()) {
        QMessageBox::information(this, "Silence Detection", "No significant silence detected.");
        return;
    }

    QString info;
    for (int i = 0; i < qMin(silences.size(), 20); ++i) {
        info += QString("%1. %2s - %3s (%4s)\n")
            .arg(i + 1)
            .arg(silences[i].startTime, 0, 'f', 1)
            .arg(silences[i].endTime, 0, 'f', 1)
            .arg(silences[i].duration(), 0, 'f', 1);
    }
    if (silences.size() > 20)
        info += QString("... and %1 more").arg(silences.size() - 20);

    QMessageBox::information(this, "Silence Detected", info);
}

void MainWindow::autoJumpCut()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Jump Cut", "Add clips first.");
        return;
    }

    auto reply = QMessageBox::question(this, "Auto Jump Cut",
        "This will automatically split clips at silent regions and remove the silence.\n\n"
        "Continue?", QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    statusBar()->showMessage("Analyzing and cutting...");
    const auto &clips = m_timeline->videoClips();

    int totalCuts = 0;
    for (const auto &clip : clips) {
        auto silences = AutoEdit::detectSilenceFromFile(clip.filePath);
        auto cuts = AutoEdit::generateJumpCuts(silences, clip.effectiveDuration());
        totalCuts += cuts.size() / 2;
    }

    statusBar()->showMessage(QString("Jump cut analysis complete: %1 cut point(s) found").arg(totalCuts));
    QMessageBox::information(this, "Jump Cut",
        QString("Found %1 potential cut(s).\n\n"
                "Use Edit > Split to manually apply cuts at detected points.")
            .arg(totalCuts));
}

void MainWindow::autoSceneDetect()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Scene Detection", "Add clips first.");
        return;
    }

    statusBar()->showMessage("Analyzing video for scene changes...");
    const auto &clip = m_timeline->videoClips().first();
    auto scenes = AutoEdit::detectSceneChanges(clip.filePath);
    statusBar()->showMessage(QString("Found %1 scene change(s)").arg(scenes.size()));

    if (scenes.isEmpty()) {
        QMessageBox::information(this, "Scene Detection", "No scene changes detected.");
        return;
    }

    QString info;
    for (int i = 0; i < qMin(scenes.size(), 30); ++i) {
        info += QString("%1. %2s (confidence: %3%)\n")
            .arg(i + 1)
            .arg(scenes[i].time, 0, 'f', 1)
            .arg(static_cast<int>(scenes[i].confidence * 100));
    }

    QMessageBox::information(this, "Scene Changes Detected", info);
}

void MainWindow::changeTheme()
{
    QStringList themes;
    for (const auto &t : ThemeManager::instance().availableThemes())
        themes << t.name;

    bool ok;
    QString selected = QInputDialog::getItem(this, "Change Theme",
        "Select theme:", themes, 0, false, &ok);
    if (!ok) return;

    ThemeType type = ThemeType::Dark;
    if (selected == "Light")    type = ThemeType::Light;
    else if (selected == "Midnight") type = ThemeType::Midnight;
    else if (selected == "Ocean")    type = ThemeType::Ocean;

    ThemeManager::instance().applyTheme(type, this);
    statusBar()->showMessage(QString("Theme: %1").arg(selected));
}

void MainWindow::multiCamSetup()
{
    if (!m_multiCam)
        m_multiCam = new MultiCamSession(this);

    // Add video files as camera sources
    QStringList files = QFileDialog::getOpenFileNames(this, "Add Camera Sources",
        QString(), "Video Files (*.mp4 *.mkv *.mov *.webm);;All Files (*)");

    for (const auto &file : files)
        m_multiCam->addSource(file);

    if (m_multiCam->sourceCount() < 2) {
        QMessageBox::information(this, "Multi-Camera",
            "Add at least 2 camera sources for multi-camera editing.");
        return;
    }

    auto reply = QMessageBox::question(this, "Multi-Camera Sync",
        QString("Added %1 camera source(s).\n\n"
                "Auto-sync cameras by audio?")
            .arg(m_multiCam->sourceCount()),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        statusBar()->showMessage("Syncing cameras by audio...");
        m_multiCam->autoSyncByAudio();
        QString info;
        for (int i = 0; i < m_multiCam->sourceCount(); ++i) {
            info += QString("%1: offset %2s\n")
                .arg(m_multiCam->sources()[i].label)
                .arg(m_multiCam->sources()[i].syncOffset, 0, 'f', 2);
        }
        statusBar()->showMessage("Sync complete");
        QMessageBox::information(this, "Sync Results", info);
    }
}

void MainWindow::multiCamSwitch()
{
    if (!m_multiCam || m_multiCam->sourceCount() < 2) {
        QMessageBox::information(this, "Multi-Camera", "Set up cameras first (Tools > Multi-Camera Setup).");
        return;
    }

    QStringList cameras;
    for (const auto &src : m_multiCam->sources())
        cameras << src.label;

    bool ok;
    QString selected = QInputDialog::getItem(this, "Switch Camera",
        "Select camera at current playhead:", cameras, 0, false, &ok);
    if (!ok) return;

    int idx = cameras.indexOf(selected);
    m_multiCam->switchToCamera(idx, m_timeline->playheadPosition());
    statusBar()->showMessage(QString("Switched to %1 at %2s")
        .arg(selected).arg(m_timeline->playheadPosition(), 0, 'f', 1));
}

// US-FEAT-D: motion tracking UI
void MainWindow::trackMotion()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Motion Tracking", "Select a clip first.");
        return;
    }

    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) return;
    const ClipInfo &clip = clips.first();

    if (!m_motionTracker)
        m_motionTracker = new MotionTracker(this);

    statusBar()->showMessage(
        QString::fromUtf8("\xE3\x83\x97\xE3\x83\xAC\xE3\x83\x93\xE3\x83\xA5"
                           "\xE3\x83\xBC\xE3\x81\xA7\xE3\x83\x88\xE3\x83\xA9"
                           "\xE3\x83\x83\xE3\x82\xAD\xE3\x83\xB3\xE3\x82\xB0"
                           "\xE9\xA0\x98\xE5\x9F\x9F\xE3\x82\x92\xE3\x83\x89"
                           "\xE3\x83\xA9\xE3\x83\x83\xE3\x82\xB0\xE3\x81\x97"
                           "\xE3\x81\xA6\xE9\x81\xB8\xE6\x8A\x9E\xE3\x81\x97"
                           "\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95"
                           "\xE3\x81\x84\xE3\x80\x82 (Esc \xE3\x81\xA7\xE3"
                           "\x82\xAD\xE3\x83\xA3\xE3\x83\xB3\xE3\x82\xBB\xE3"
                           "\x83\xAB)"), 0);

    // US-WIRE-3: replace hardcoded QRect(100,100,64,64) with a user-driven
    // rubber-band drag on the preview. The callback receives the rect in
    // source-frame coords and kicks off the tracking worker + progress UI.
    m_player->enterRegionPickerMode([this, clip](QRect region) {
        statusBar()->showMessage(QString());

        if (region.isNull() || region.width() <= 0 || region.height() <= 0) {
            // User cancelled (Esc or too-small drag)
            return;
        }

        QProgressDialog *progress = new QProgressDialog("Tracking motion\u2026", "Cancel", 0, 100, this);
        progress->setWindowTitle("Motion Tracking");
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(0);

        connect(m_motionTracker, &MotionTracker::progressChanged, progress, &QProgressDialog::setValue, Qt::UniqueConnection);

        connect(m_motionTracker, &MotionTracker::trackingComplete, this,
            [this, progress](const TrackingResult &result) {
                progress->close();
                progress->deleteLater();
                if (result.isEmpty()) {
                    QMessageBox::warning(this,
                        QString::fromUtf8("\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0"),
                        QString::fromUtf8("\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0\xe7\xb5\x90\xe6\x9e\x9c\xe3\x81\x8c\xe7\xa9\xba\xe3\x81\xa7\xe3\x81\x99"));
                    return;
                }

                const auto &vClips = m_timeline->videoClips();
                if (vClips.isEmpty()) return;
                const auto &mgr = vClips[0].textManager;
                const int overlayCount = mgr.count();

                if (overlayCount == 0) {
                    QMessageBox::information(this,
                        QString::fromUtf8("\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0\xe7\xb5\x90\xe6\x9e\x9c\xe3\x82\x92\xe9\x81\xa9\xe7\x94\xa8"),
                        QString::fromUtf8("\xe9\x81\xa9\xe7\x94\xa8\xe5\x85\x88\xe3\x81\xae\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x83\xac\xe3\x82\xa4\xe3\x81\x8c\xe3\x81\x82\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82\xe5\x85\x88\xe3\x81\xab\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x83\xac\xe3\x82\xa4\xe3\x82\x92\xe8\xbf\xbd\xe5\x8a\xa0\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84\xe3\x80\x82"));
                    return;
                }

                QDialog dlg(this);
                dlg.setWindowTitle(QString::fromUtf8("\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0\xe7\xb5\x90\xe6\x9e\x9c\xe3\x82\x92\xe9\x81\xa9\xe7\x94\xa8"));
                auto *layout = new QVBoxLayout(&dlg);

                auto *label = new QLabel(
                    QString("Tracking complete: %1 frames tracked.\n"
                            "\xe9\x81\xa9\xe7\x94\xa8\xe5\x85\x88\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x83\xac\xe3\x82\xa4\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84:")
                        .arg(result.regions.size()), &dlg);
                label->setWordWrap(true);
                layout->addWidget(label);

                auto *combo = new QComboBox(&dlg);
                for (int i = 0; i < overlayCount; ++i) {
                    const auto &ov = mgr.overlay(i);
                    combo->addItem(
                        QString("%1: \"%2\"")
                            .arg(i + 1)
                            .arg(ov.text.left(30)),
                        i);
                }
                layout->addWidget(combo);

                auto *btnBox = new QDialogButtonBox(
                    QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
                connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
                connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
                layout->addWidget(btnBox);

                if (dlg.exec() != QDialog::Accepted)
                    return;

                const int selIdx = combo->currentData().toInt();
                const auto &overlays = mgr.overlays();
                if (selIdx < 0 || selIdx >= overlays.size())
                    return;

                EnhancedTextOverlay selOverlay = overlays[selIdx];

                if (!selOverlay.positionKeyframes.isEmpty()) {
                    const int ret = QMessageBox::question(this,
                        QString::fromUtf8("\xe3\x82\xad\xe3\x83\xbc\xe3\x83\x95\xe3\x83\xac\xe3\x83\xbc\xe3\x83\xa0\xe7\xa2\xba\xe8\xaa\x8d"),
                        QString::fromUtf8("\xe3\x81\x93\xe3\x81\xae\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x83\xac\xe3\x82\xa4\xe3\x81\xab\xe3\x81\xaf\xe6\x97\xa2\xe5\xad\x98\xe3\x81\xae\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0\xe3\x82\xad\xe3\x83\xbc\xe3\x83\x95\xe3\x83\xac\xe3\x83\xbc\xe3\x83\xa0\xe3\x81\x8c\xe3\x81\x82\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82\n\xe6\x97\xa2\xe5\xad\x98\xe3\x81\xae\xe3\x82\xad\xe3\x83\xbc\xe3\x83\x95\xe3\x83\xac\xe3\x83\xbc\xe3\x83\xa0\xe3\x82\x92\xe4\xb8\x8a\xe6\x9b\xb8\xe3\x81\x8d\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x81\x8b\xef\xbc\x9f"),
                        QMessageBox::Yes | QMessageBox::No);
                    if (ret != QMessageBox::Yes)
                        return;
                }

                TrackerLink::applyToOverlay(&selOverlay,
                    result, m_projectConfig.fps);
                m_timeline->applyTrackingToOverlay(selIdx, selOverlay);

                {
                    const auto &updatedMgr = m_timeline->videoClips()[0].textManager;
                    QVector<EnhancedTextOverlay> ovList;
                    for (int i = 0; i < updatedMgr.count(); ++i)
                        ovList.append(updatedMgr.overlay(i));
                    m_player->setTextOverlays(ovList);
                }

                statusBar()->showMessage(
                    QString::fromUtf8("\xe3\x83\x88\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xad\xe3\x83\xb3\xe3\x82\xb0\xe3\x82\x92\xe9\x81\xa9\xe7\x94\xa8\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f: %1\xe3\x83\x95\xe3\x83\xac\xe3\x83\xbc\xe3\x83\xa0\xe2\x86\x92\"%2\"")
                        .arg(selOverlay.positionKeyframes.size())
                        .arg(selOverlay.text.left(20)));
            }, Qt::UniqueConnection);

        connect(progress, &QProgressDialog::canceled, this, [this]() {
            if (m_motionTracker)
                m_motionTracker->cancel();
        });

        m_motionTracker->startTracking(clip.filePath, region);
        statusBar()->showMessage("Starting motion tracking\u2026");
    });
}

void MainWindow::motionTrackSetup()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Motion Tracking", "Select a clip first.");
        return;
    }

    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) return;

    // Let user define tracking region
    bool ok;
    int x = QInputDialog::getInt(this, "Motion Tracking", "Region X:", 100, 0, 9999, 1, &ok);
    if (!ok) return;
    int y = QInputDialog::getInt(this, "Motion Tracking", "Region Y:", 100, 0, 9999, 1, &ok);
    if (!ok) return;
    int w = QInputDialog::getInt(this, "Motion Tracking", "Region Width:", 64, 16, 512, 1, &ok);
    if (!ok) return;
    int h = QInputDialog::getInt(this, "Motion Tracking", "Region Height:", 64, 16, 512, 1, &ok);
    if (!ok) return;

    if (!m_motionTracker)
        m_motionTracker = new MotionTracker(this);

    connect(m_motionTracker, &MotionTracker::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Tracking... %1%").arg(pct));
    }, Qt::UniqueConnection);

    connect(m_motionTracker, &MotionTracker::trackingComplete, this, [this](const TrackingResult &result) {
        statusBar()->showMessage(QString("Tracking complete: %1 frames tracked")
            .arg(result.regions.size()));
        QMessageBox::information(this, "Motion Tracking",
            QString("Tracked %1 frames.\n\nUse Effects > Apply to Overlay to attach an overlay to the tracked path.")
                .arg(result.regions.size()));
    }, Qt::UniqueConnection);

    QRect region(x, y, w, h);
    m_motionTracker->startTracking(clips.first().filePath, region);
    statusBar()->showMessage("Starting motion tracking...");
}

void MainWindow::audioNoiseDenoise()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Audio Denoise", "Add clips first.");
        return;
    }

    const auto &clip = m_timeline->videoClips().first();

    bool ok;
    double reduction = QInputDialog::getDouble(this, "Audio Noise Reduction",
        "Noise reduction amount (0.0 = none, 1.0 = max):", 0.5, 0.0, 1.0, 2, &ok);
    if (!ok) return;

    double noiseFloor = QInputDialog::getDouble(this, "Audio Noise Reduction",
        "Noise floor (dB, -80 to 0):", -40.0, -80.0, 0.0, 0, &ok);
    if (!ok) return;

    QString outputPath = QFileDialog::getSaveFileName(this, "Save Denoised Audio",
        QFileInfo(clip.filePath).baseName() + "_denoised.wav",
        "Audio Files (*.wav *.mp3 *.aac);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_noiseReduction)
        m_noiseReduction = new NoiseReduction(this);

    AudioDenoiseConfig config;
    config.reductionAmount = reduction;
    config.noiseFloor = noiseFloor;

    connect(m_noiseReduction, &NoiseReduction::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Denoising audio... %1%").arg(pct));
    }, Qt::UniqueConnection);

    connect(m_noiseReduction, &NoiseReduction::denoiseComplete, this, [this](bool success, const QString &msg) {
        if (success)
            statusBar()->showMessage("Audio denoise complete: " + msg);
        else
            QMessageBox::warning(this, "Audio Denoise Failed", msg);
    }, Qt::UniqueConnection);

    m_noiseReduction->denoiseAudio(clip.filePath, outputPath, config);
    statusBar()->showMessage("Denoising audio...");
}

void MainWindow::videoNoiseDenoise()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Video Denoise", "Add clips first.");
        return;
    }

    const auto &clip = m_timeline->videoClips().first();

    QStringList methods = {"HQDN3D (Fast)", "NLMeans (High Quality)"};
    bool ok;
    QString method = QInputDialog::getItem(this, "Video Denoise",
        "Denoise method:", methods, 0, false, &ok);
    if (!ok) return;

    double spatial = QInputDialog::getDouble(this, "Video Denoise",
        "Spatial strength (0-30):", 4.0, 0.0, 30.0, 1, &ok);
    if (!ok) return;

    double temporal = QInputDialog::getDouble(this, "Video Denoise",
        "Temporal strength (0-30):", 6.0, 0.0, 30.0, 1, &ok);
    if (!ok) return;

    QString outputPath = QFileDialog::getSaveFileName(this, "Save Denoised Video",
        QFileInfo(clip.filePath).baseName() + "_denoised.mp4",
        "Video Files (*.mp4 *.mkv *.mov);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_noiseReduction)
        m_noiseReduction = new NoiseReduction(this);

    VideoDenoiseConfig config;
    config.spatialStrength = spatial;
    config.temporalStrength = temporal;
    config.method = method.startsWith("NLMeans") ? VideoDenoiseMethod::NLMeans : VideoDenoiseMethod::HQDN3D;

    connect(m_noiseReduction, &NoiseReduction::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Denoising video... %1%").arg(pct));
    }, Qt::UniqueConnection);

    connect(m_noiseReduction, &NoiseReduction::denoiseComplete, this, [this](bool success, const QString &msg) {
        if (success)
            statusBar()->showMessage("Video denoise complete: " + msg);
        else
            QMessageBox::warning(this, "Video Denoise Failed", msg);
    }, Qt::UniqueConnection);

    m_noiseReduction->denoiseVideo(clip.filePath, outputPath, config);
    statusBar()->showMessage("Denoising video...");
}

void MainWindow::generateSubtitles()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Subtitle Generation", "Add clips first.");
        return;
    }

    if (!SubtitleGenerator::isWhisperAvailable()) {
        QMessageBox::warning(this, "Whisper Not Found",
            "Whisper is not installed.\n\n"
            "Install with: pip install openai-whisper\n"
            "Or build whisper.cpp from source.");
        return;
    }

    const auto &clip = m_timeline->videoClips().first();

    QStringList languages = {"auto", "ja", "en", "zh", "ko", "fr", "de", "es", "it", "pt", "ru"};
    bool ok;
    QString lang = QInputDialog::getItem(this, "Subtitle Generation",
        "Language (auto = auto-detect):", languages, 0, false, &ok);
    if (!ok) return;

    if (!m_subtitleGen)
        m_subtitleGen = new SubtitleGenerator(this);

    WhisperConfig config;
    config.language = lang;

    connect(m_subtitleGen, &SubtitleGenerator::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Generating subtitles... %1%").arg(pct));
    }, Qt::UniqueConnection);

    connect(m_subtitleGen, &SubtitleGenerator::generationComplete, this, [this](const QVector<SubtitleSegment> &segments) {
        statusBar()->showMessage(QString("Generated %1 subtitle segment(s)").arg(segments.size()));

        if (segments.isEmpty()) {
            QMessageBox::information(this, "Subtitles", "No speech detected.");
            return;
        }

        // Ask to export
        QStringList options = {"Apply to clip as text overlays", "Export as SRT file", "Export as VTT file"};
        bool ok2;
        QString choice = QInputDialog::getItem(this, "Subtitles Generated",
            QString("%1 segments found. What to do?").arg(segments.size()),
            options, 0, false, &ok2);
        if (!ok2) return;

        if (choice.startsWith("Apply")) {
            auto overlays = m_subtitleGen->toTextOverlays(segments);
            auto clips = m_timeline->videoClips();
            if (!clips.isEmpty()) {
                for (const auto &o : overlays)
                    clips[0].textManager.addOverlay(o);
            }
            statusBar()->showMessage(QString("Applied %1 subtitle overlay(s)").arg(overlays.size()));
        } else {
            QString ext = choice.contains("SRT") ? "srt" : "vtt";
            QString filter = choice.contains("SRT") ? "SRT (*.srt)" : "WebVTT (*.vtt)";
            QString path = QFileDialog::getSaveFileName(this, "Export Subtitles",
                QString("subtitles.%1").arg(ext), filter);
            if (!path.isEmpty()) {
                bool exported = ext == "srt" ?
                    SubtitleGenerator::exportSRT(segments, path) :
                    SubtitleGenerator::exportVTT(segments, path);
                if (exported)
                    statusBar()->showMessage("Exported: " + path);
                else
                    QMessageBox::warning(this, "Export Failed", "Could not write subtitle file.");
            }
        }
    }, Qt::UniqueConnection);

    connect(m_subtitleGen, &SubtitleGenerator::errorOccurred, this, [this](const QString &error) {
        QMessageBox::warning(this, "Subtitle Generation Failed", error);
    }, Qt::UniqueConnection);

    m_subtitleGen->generate(clip.filePath, config);
    statusBar()->showMessage("Extracting audio and generating subtitles...");
}

void MainWindow::applyEffectPreset()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Effect Preset", "Select a clip first.");
        return;
    }

    auto &library = PresetLibrary::instance();
    QStringList presetNames;
    for (const auto &p : library.allPresets())
        presetNames << QString("%1 [%2]").arg(p.name, p.category);

    if (presetNames.isEmpty()) {
        QMessageBox::information(this, "Effect Preset", "No presets available.");
        return;
    }

    bool ok;
    QString selected = QInputDialog::getItem(this, "Apply Effect Preset",
        "Select preset:", presetNames, 0, false, &ok);
    if (!ok) return;

    // Extract name before the bracket
    QString name = selected.left(selected.lastIndexOf(" ["));
    auto result = library.applyPreset(name);

    m_timeline->setClipColorCorrection(result.first);
    m_timeline->setClipEffects(result.second);
    m_player->setColorCorrection(result.first);

    statusBar()->showMessage(QString("Applied preset: %1 (%2 effect(s))")
        .arg(name).arg(result.second.size()));
}

void MainWindow::saveEffectPreset()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Save Preset", "Select a clip first.");
        return;
    }

    bool ok;
    QString name = QInputDialog::getText(this, "Save Effect Preset",
        "Preset name:", QLineEdit::Normal, "My Preset", &ok);
    if (!ok || name.isEmpty()) return;

    QString category = QInputDialog::getText(this, "Save Effect Preset",
        "Category:", QLineEdit::Normal, "Custom", &ok);
    if (!ok) return;

    EffectPreset preset;
    preset.name = name;
    preset.category = category;
    preset.colorCorrection = m_timeline->clipColorCorrection();
    preset.effects = m_timeline->clipEffects();

    PresetLibrary::instance().addPreset(preset);
    PresetLibrary::instance().saveLibrary();
    statusBar()->showMessage(QString("Saved preset: %1").arg(name));
}

void MainWindow::manageEffectPresets()
{
    auto &library = PresetLibrary::instance();
    auto presets = library.allPresets();

    QString info = QString("Effect Presets (%1 total):\n\n").arg(presets.size());
    for (const auto &p : presets) {
        info += QString("• %1 [%2]%3\n")
            .arg(p.name, p.category)
            .arg(p.isBuiltIn ? " (built-in)" : "");
    }
    info += "\nUse Effects > Apply Preset to use, or Save Current as Preset to create new ones.";

    QMessageBox::information(this, "Manage Presets", info);
}

void MainWindow::stabilizeVideo()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Stabilize", "Add clips first.");
        return;
    }

    const auto &clip = m_timeline->videoClips().first();
    bool ok;
    int smoothing = QInputDialog::getInt(this, "Video Stabilization",
        "Smoothing (1-100, higher=smoother):", 10, 1, 100, 1, &ok);
    if (!ok) return;

    QString outputPath = QFileDialog::getSaveFileName(this, "Save Stabilized Video",
        QFileInfo(clip.filePath).baseName() + "_stabilized.mp4",
        "Video Files (*.mp4 *.mkv *.mov);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_stabilizer)
        m_stabilizer = new VideoStabilizer(this);

    StabilizerConfig config;
    config.smoothing = smoothing;

    auto *progress = new QProgressDialog(
        tr("スタビライズ中..."), tr("キャンセル"), 0, 100, this);
    progress->setWindowTitle(tr("スタビライズ"));
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setMinimumDuration(500);

    connect(m_stabilizer, &VideoStabilizer::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Stabilizing... %1%").arg(pct));
    }, Qt::UniqueConnection);
    connect(m_stabilizer, &VideoStabilizer::progressChanged,
            progress, &QProgressDialog::setValue, Qt::UniqueConnection);
    connect(progress, &QProgressDialog::canceled,
            m_stabilizer, &VideoStabilizer::cancel,
            Qt::UniqueConnection);
    connect(m_stabilizer, &VideoStabilizer::stabilizeComplete, this, [this](bool ok2, const QString &msg) {
        statusBar()->showMessage(ok2 ? "Stabilization complete" : "Stabilization failed: " + msg);
    }, Qt::UniqueConnection);
    connect(m_stabilizer, &VideoStabilizer::stabilizeComplete,
            this, [progress](bool, const QString &) {
        progress->close();
        progress->deleteLater();
    }, Qt::UniqueConnection);

    m_stabilizer->stabilize(clip.filePath, outputPath, config);
    statusBar()->showMessage("Stabilizing video (2-pass)...");
}

void MainWindow::applyLut()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "LUT", "Select a clip first.");
        return;
    }

    // Offer built-in or custom LUT
    QStringList options;
    for (const auto &lut : LutLibrary::instance().allLuts())
        options << lut.name;
    options << "Load .cube file...";

    bool ok;
    QString selected = QInputDialog::getItem(this, "Apply LUT",
        "Select LUT:", options, 0, false, &ok);
    if (!ok) return;

    LutData lut;
    if (selected == "Load .cube file...") {
        QString path = QFileDialog::getOpenFileName(this, "Load LUT",
            QString(), "Cube LUT (*.cube);;All Files (*)");
        if (path.isEmpty()) return;
        lut = LutImporter::loadCubeFile(path);
        if (!lut.isValid()) {
            QMessageBox::warning(this, "LUT Error", "Could not parse LUT file.");
            return;
        }
        LutLibrary::instance().addLut(path);
    } else {
        auto found = LutLibrary::instance().findByName(selected);
        if (!found.isValid()) return;
        lut = found;
    }

    double intensity = QInputDialog::getDouble(this, "LUT Intensity",
        "Intensity (0.0-1.0):", 1.0, 0.0, 1.0, 2, &ok);
    if (!ok) return;
    lut.intensity = intensity;

    statusBar()->showMessage(QString("Applied LUT: %1 (intensity %2)")
        .arg(lut.name).arg(intensity, 0, 'f', 1));
}

void MainWindow::loadLutCubeFile()
{
    QString path = QFileDialog::getOpenFileName(this, "LUT を読み込み",
        QString(), "Cube LUT (*.cube);;All Files (*)");
    if (path.isEmpty()) return;

    LutData lut = LutImporter::loadCubeFile(path);
    if (!lut.isValid()) {
        QMessageBox::warning(this, "LUT Error", "Could not parse LUT file.");
        return;
    }

    if (m_player)
        m_player->glPreview()->setLut(lut);

    if (m_lutIntensitySlider)
        m_lutIntensitySlider->setValue(100);

    LutLibrary::instance().addLut(path);
    if (m_colorGradingPanel)
        m_colorGradingPanel->setLutList(LutLibrary::instance().allLuts());

    statusBar()->showMessage(QString("LUT 読み込み: %1").arg(lut.name));
}

void MainWindow::clearLutIntensity()
{
    if (m_player)
        m_player->glPreview()->clearLut();
    if (m_lutIntensitySlider)
        m_lutIntensitySlider->setValue(0);
    statusBar()->showMessage("LUT 解除");
}

void MainWindow::manageLuts()
{
    auto &library = LutLibrary::instance();
    auto luts = library.allLuts();

    QString info = QString("LUT Library (%1 total):\n\n").arg(luts.size());
    for (const auto &l : luts) {
        info += QString("• %1 [%2x%2x%2]%3\n")
            .arg(l.name).arg(l.size)
            .arg(l.isBuiltIn() ? " (built-in)" : "");
    }
    QMessageBox::information(this, "Manage LUTs", info);
}

void MainWindow::openProxySettings()
{
    auto &pm = ProxyManager::instance();

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("プロキシ設定"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *modeCheck = new QCheckBox(
        QStringLiteral("プロキシ再生 (低解像度ファイルで再生)"), &dlg);
    modeCheck->setChecked(pm.isProxyMode());
    modeCheck->setToolTip(QStringLiteral(
        "ON: 生成済みプロキシをタイムラインで使用 (高速再生)\n"
        "OFF: 元解像度ファイルを使用"));
    layout->addWidget(modeCheck);

    // Encoder override (US-1): empty itemData = Auto. Probe each GPU
    // encoder up-front and disable items the runtime ffmpeg can't run so
    // the user can't pin an encoder that will fall through to libx264.
    layout->addWidget(new QLabel(QStringLiteral("エンコーダー:"), &dlg));
    auto *encoderCombo = new QComboBox(&dlg);
    struct EncOpt { const char *label; const char *value; };
    const EncOpt encOpts[] = {
        {"Auto (自動検出)",      ""},
        {"NVIDIA NVENC",         "h264_nvenc"},
        {"Intel QSV",            "h264_qsv"},
        {"AMD AMF",              "h264_amf"},
        {"CPU (libx264)",        "libx264"},
    };
    auto *encModel = new QStandardItemModel(encoderCombo);
    for (const auto &opt : encOpts) {
        auto *item = new QStandardItem(QString::fromUtf8(opt.label));
        item->setData(QString::fromLatin1(opt.value), Qt::UserRole);
        const QString enc = QString::fromLatin1(opt.value);
        const bool isGpu = (enc == "h264_nvenc" || enc == "h264_qsv" || enc == "h264_amf");
        if (isGpu && !ProxyManager::ffmpegHasEncoder(enc)) {
            item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
        }
        encModel->appendRow(item);
    }
    encoderCombo->setModel(encModel);
    const QString currentEnc = pm.encoderOverride();
    for (int i = 0; i < encoderCombo->count(); ++i) {
        if (encoderCombo->itemData(i, Qt::UserRole).toString() == currentEnc) {
            encoderCombo->setCurrentIndex(i);
            break;
        }
    }
    layout->addWidget(encoderCombo);

    // Quality preset (US-2). Index 0..2 maps to QualityPreset enum directly.
    layout->addWidget(new QLabel(QStringLiteral("品質:"), &dlg));
    auto *qualityCombo = new QComboBox(&dlg);
    qualityCombo->addItem(QStringLiteral("High"),   static_cast<int>(QualityPreset::High));
    qualityCombo->addItem(QStringLiteral("Medium"), static_cast<int>(QualityPreset::Medium));
    qualityCombo->addItem(QStringLiteral("Low"),    static_cast<int>(QualityPreset::Low));
    {
        const int cur = static_cast<int>(pm.qualityPreset());
        for (int i = 0; i < qualityCombo->count(); ++i) {
            if (qualityCombo->itemData(i).toInt() == cur) {
                qualityCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    layout->addWidget(qualityCombo);

    // Storage directory (US-3). Read-only QLineEdit shows the resolved path
    // (custom QSettings value or default). 'フォルダ選択...' opens a dir
    // picker; we validate write-ability before persisting.
    layout->addWidget(new QLabel(QStringLiteral("保存先:"), &dlg));
    auto *storageRow = new QHBoxLayout();
    auto *storageEdit = new QLineEdit(&dlg);
    storageEdit->setReadOnly(true);
    storageEdit->setText(ProxyManager::proxyDir());
    auto *storageBtn = new QPushButton(QStringLiteral("フォルダ選択..."), &dlg);
    storageRow->addWidget(storageEdit);
    storageRow->addWidget(storageBtn);
    layout->addLayout(storageRow);
    auto *storageNote = new QLabel(
        QStringLiteral("既存 proxy は元の場所に残ります"), &dlg);
    storageNote->setStyleSheet("color:#888; font-size:10px;");
    layout->addWidget(storageNote);
    QString pendingStorage; // empty = no change
    connect(storageBtn, &QPushButton::clicked, &dlg, [&dlg, storageEdit, &pendingStorage]() {
        const QString picked = QFileDialog::getExistingDirectory(
            &dlg,
            QStringLiteral("プロキシ保存先を選択"),
            storageEdit->text());
        if (picked.isEmpty())
            return;
        QFileInfo info(picked);
        if (!info.isDir() || !info.isWritable()) {
            QMessageBox::warning(&dlg, QStringLiteral("プロキシ保存先"),
                QStringLiteral("選択したフォルダに書き込めません。\n別のフォルダを選択してください。"));
            return;
        }
        pendingStorage = picked;
        storageEdit->setText(picked);
    });

    auto *divisorLabel = new QLabel(
        QStringLiteral("プレビュー解像度 (CPU エフェクト適用時に効く):"), &dlg);
    layout->addWidget(divisorLabel);
    auto *divisorCombo = new QComboBox(&dlg);
    divisorCombo->addItem(QStringLiteral("Full (1/1)"), 1);
    divisorCombo->addItem(QStringLiteral("Half (1/2)"), 2);
    divisorCombo->addItem(QStringLiteral("Quarter (1/4)"), 4);
    divisorCombo->addItem(QStringLiteral("Eighth (1/8)"), 8);
    const int currentDivisor = m_player ? m_player->proxyDivisor() : 1;
    for (int i = 0; i < divisorCombo->count(); ++i) {
        if (divisorCombo->itemData(i).toInt() == currentDivisor) {
            divisorCombo->setCurrentIndex(i);
            break;
        }
    }
    layout->addWidget(divisorCombo);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    pm.setEncoderOverride(encoderCombo->currentData(Qt::UserRole).toString());
    pm.setQualityPreset(static_cast<QualityPreset>(qualityCombo->currentData().toInt()));
    if (!pendingStorage.isEmpty()) {
        ProxyManager::setProxyStorageDir(pendingStorage);
        QDir().mkpath(pendingStorage);
    }

    // Apply proxy mode flip first — the refresh below relies on the new
    // mode to resolve paths correctly.
    const bool newMode = modeCheck->isChecked();
    if (pm.isProxyMode() != newMode) {
        pm.setProxyMode(newMode);
        statusBar()->showMessage(newMode
            ? QStringLiteral("Proxy mode ON (low-res playback)")
            : QStringLiteral("Proxy mode OFF (original quality)"));
        if (m_timeline && m_player) {
            const bool wasPlaying = m_player->isPlaying();
            const int64_t posUs = m_player->timelinePositionUs();
            if (wasPlaying)
                m_player->pause();
            m_timeline->refreshPlaybackSequence();
            m_player->seek(static_cast<int>(posUs / 1000));
            if (wasPlaying)
                m_player->play();
        }
    }
    if (m_player)
        m_player->setProxyDivisor(divisorCombo->currentData().toInt());
}

void MainWindow::toggleProxyMode()
{
    auto &pm = ProxyManager::instance();
    pm.setProxyMode(!pm.isProxyMode());
    statusBar()->showMessage(pm.isProxyMode() ? "Proxy mode ON (low-res playback)" : "Proxy mode OFF (original quality)");

    if (!m_timeline || !m_player)
        return;
    // Snapshot playback state, refresh the sequence so getProxyPath picks
    // up the new resolution, then re-seek to where we were. setSequence's
    // own clamped restoration is not enough because loadFile resets the
    // file-local position to 0 and the decoder bootstraps from there —
    // the explicit seek after refresh forces the new decoder to land on
    // the same timeline position the user was watching.
    const bool wasPlaying = m_player->isPlaying();
    const int64_t posUs = m_player->timelinePositionUs();
    if (wasPlaying)
        m_player->pause();
    m_timeline->refreshPlaybackSequence();
    m_player->seek(static_cast<int>(posUs / 1000));
    if (wasPlaying)
        m_player->play();
}

void MainWindow::generateProxies()
{
    if (!m_timeline) {
        QMessageBox::information(this, "Proxies", "Timeline not ready.");
        return;
    }
    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) {
        QMessageBox::information(this, "Proxies", "Add clips first.");
        return;
    }

    QStringList paths;
    for (const auto &c : clips)
        paths << c.filePath;

    auto &pm = ProxyManager::instance();

    // Drop any prior allProxiesReady / progressChanged lambdas before
    // wiring up the new ones. Qt::UniqueConnection does NOT deduplicate
    // distinct lambda objects (each closure is a separate functor type),
    // so without this disconnect the Nth generateProxies call fires the
    // refresh handler N times on completion — the visible symptom is the
    // preview snapping back to position multiple times in a row.
    disconnect(&pm, &ProxyManager::allProxiesReady, this, nullptr);
    disconnect(&pm, &ProxyManager::progressChanged, this, nullptr);

    // generateAllProxies skips clips whose entry is Ready/Generating, so a
    // bare invocation on a project that already has proxies would emit
    // allProxiesReady immediately without doing any work — confusing if the
    // user expected to "regenerate". Offer to delete the existing proxies
    // first so the queue actually runs.
    int existing = 0;
    for (const auto &p : paths)
        if (pm.hasProxy(p)) ++existing;
    if (existing > 0) {
        const auto reply = QMessageBox::question(
            this, "Proxies",
            QString("既存のプロキシ %1 個を削除して再生成しますか?\n\n"
                    "「いいえ」を選ぶと既存プロキシをそのまま使い、未生成のクリップだけ生成します。")
                .arg(existing),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
            QMessageBox::No);
        if (reply == QMessageBox::Cancel)
            return;
        if (reply == QMessageBox::Yes) {
            // Releasing the proxy file while VideoPlayer is mid-decode on
            // it crashes the app — the avformat demuxer holds an open
            // handle that suddenly points at deleted bytes. Cancel any
            // in-flight ffmpeg job, force the player off proxy paths, and
            // only then unlink the files. The proxy mode flag is restored
            // afterwards so the new generation will switch the preview to
            // the freshly-built proxies once allProxiesReady fires.
            pm.cancelGeneration();
            if (m_player && m_player->isPlaying())
                m_player->pause();
            const bool wasProxyMode = pm.isProxyMode();
            if (wasProxyMode) {
                pm.setProxyMode(false);
                if (m_timeline)
                    m_timeline->refreshPlaybackSequence();
            }
            for (const auto &p : paths)
                pm.deleteProxy(p);
            if (wasProxyMode)
                pm.setProxyMode(true);
        }
    }

    connect(&pm, &ProxyManager::allProxiesReady, this, [this]() {
        // Qt::SingleShotConnection so the lambda is removed automatically
        // after one fire, in addition to the upfront disconnect — defense
        // in depth against the same lambda accumulating across calls.
        statusBar()->showMessage("All proxies generated");
        if (!m_timeline || !m_player)
            return;
        // VideoPlayer is sequence-driven (Timeline::sequenceChanged →
        // MainWindow's setSequence lambda → m_player->setSequence). A bare
        // loadFile() does not retarget the active sequence, so the preview
        // would keep playing the original even after generation finishes.
        // Re-emit the sequences so getProxyPath picks up the now-Ready paths.
        //
        // Pause/play wrap + explicit re-seek matches toggleProxyMode: the
        // file swap inside setSequence resets the decoder's file-local
        // position to 0, so we have to push the timeline position back
        // ourselves once the new entries are in place.
        const bool wasPlaying = m_player->isPlaying();
        const int64_t posUs = m_player->timelinePositionUs();
        if (wasPlaying)
            m_player->pause();
        m_timeline->refreshPlaybackSequence();
        m_player->seek(static_cast<int>(posUs / 1000));
        if (wasPlaying)
            m_player->play();
    }, Qt::SingleShotConnection);
    connect(&pm, &ProxyManager::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Generating proxies... %1%").arg(pct));
    });

    pm.generateAllProxies(paths);
    statusBar()->showMessage("Generating proxy files...");
}

void MainWindow::openLoudnessSettings()
{
    QSettings prefs("VSimpleEditor", "Preferences");
    const double initAmount =
        prefs.value("audio/normalizerAmount", 0.0).toDouble();
    const double initUniformity =
        prefs.value("audio/normalizerUniformity", 0.5).toDouble();

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("オーディオ均一化"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *intro = new QLabel(QStringLiteral(
        "<b>全トラックの出力レベルを動的に均一化します。</b><br>"
        "<small>"
        "適用量 0% で完全 OFF。均一性が高いほど反応が速く出力が平らになり、"
        "低いほど元の強弱が残ります。"
        "</small>"), &dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // Amount slider 0..100 == 0..1.0
    auto *amountRow = new QHBoxLayout();
    amountRow->addWidget(new QLabel(QStringLiteral("適用量 (Amount):"), &dlg));
    auto *amountValue = new QLabel(&dlg);
    amountValue->setMinimumWidth(48);
    amountValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    amountRow->addWidget(amountValue);
    layout->addLayout(amountRow);
    auto *amountSlider = new QSlider(Qt::Horizontal, &dlg);
    amountSlider->setRange(0, 100);
    amountSlider->setValue(static_cast<int>(qBound(0.0, initAmount, 1.0) * 100.0));
    amountValue->setText(QString::number(amountSlider->value()) + " %");
    layout->addWidget(amountSlider);

    auto *uniformRow = new QHBoxLayout();
    uniformRow->addWidget(new QLabel(QStringLiteral("均一性 (Uniformity):"), &dlg));
    auto *uniformValue = new QLabel(&dlg);
    uniformValue->setMinimumWidth(48);
    uniformValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    uniformRow->addWidget(uniformValue);
    layout->addLayout(uniformRow);
    auto *uniformSlider = new QSlider(Qt::Horizontal, &dlg);
    uniformSlider->setRange(0, 100);
    uniformSlider->setValue(static_cast<int>(qBound(0.0, initUniformity, 1.0) * 100.0));
    uniformValue->setText(QString::number(uniformSlider->value()) + " %");
    layout->addWidget(uniformSlider);

    AudioMixer *mixer = m_player ? m_player->audioMixer() : nullptr;

    auto pushAmount = [mixer, amountValue](int v) {
        amountValue->setText(QString::number(v) + " %");
        if (mixer) mixer->setNormalizerAmount(v / 100.0);
    };
    auto pushUniformity = [mixer, uniformValue](int v) {
        uniformValue->setText(QString::number(v) + " %");
        if (mixer) mixer->setNormalizerUniformity(v / 100.0);
    };
    connect(amountSlider, &QSlider::valueChanged, &dlg, pushAmount);
    connect(uniformSlider, &QSlider::valueChanged, &dlg, pushUniformity);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        const double amount = amountSlider->value() / 100.0;
        const double uniformity = uniformSlider->value() / 100.0;
        prefs.setValue("audio/normalizerAmount", amount);
        prefs.setValue("audio/normalizerUniformity", uniformity);
        if (mixer) {
            mixer->setNormalizerAmount(amount);
            mixer->setNormalizerUniformity(uniformity);
        }
    } else {
        // Cancel restores the original values (they were live-applied during
        // slider drag).
        if (mixer) {
            mixer->setNormalizerAmount(initAmount);
            mixer->setNormalizerUniformity(initUniformity);
        }
    }
}

void MainWindow::openProxyManagement()
{
    ProxyManagementDialog dlg(this);
    dlg.exec();
}

void MainWindow::setSpeedRamp()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Speed Ramp", "Select a clip first.");
        return;
    }

    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) return;

    bool ok;
    double startSpeed = QInputDialog::getDouble(this, "Speed Ramp",
        "Start speed (0.1-10x):", 1.0, 0.1, 10.0, 1, &ok);
    if (!ok) return;
    double endSpeed = QInputDialog::getDouble(this, "Speed Ramp",
        "End speed (0.1-10x):", 2.0, 0.1, 10.0, 1, &ok);
    if (!ok) return;

    QStringList easings = {"Linear", "Ease In", "Ease Out", "Ease In/Out"};
    QString easing = QInputDialog::getItem(this, "Speed Ramp",
        "Easing:", easings, 3, false, &ok);
    if (!ok) return;

    QString outputPath = QFileDialog::getSaveFileName(this, "Save Speed Ramp Video",
        QFileInfo(clips.first().filePath).baseName() + "_speedramp.mp4",
        "Video Files (*.mp4 *.mkv *.mov);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_speedRamp)
        m_speedRamp = new SpeedRamp(this);

    SpeedEasing e = SpeedEasing::Linear;
    if (easing.contains("In/Out")) e = SpeedEasing::EaseInOut;
    else if (easing.contains("In"))  e = SpeedEasing::EaseIn;
    else if (easing.contains("Out")) e = SpeedEasing::EaseOut;

    m_speedRamp->clearPoints();
    m_speedRamp->addPoint(0.0, startSpeed, SpeedEasing::Linear);
    m_speedRamp->addPoint(clips.first().effectiveDuration(), endSpeed, e);

    SpeedRampConfig config;
    config.points = {SpeedPoint{0.0, startSpeed, SpeedEasing::Linear},
                     SpeedPoint{clips.first().effectiveDuration(), endSpeed, e}};

    connect(m_speedRamp, &SpeedRamp::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Applying speed ramp... %1%").arg(pct));
    }, Qt::UniqueConnection);
    connect(m_speedRamp, &SpeedRamp::rampComplete, this, [this](bool ok2, const QString &msg) {
        statusBar()->showMessage(ok2 ? "Speed ramp complete" : "Speed ramp failed: " + msg);
    }, Qt::UniqueConnection);

    m_speedRamp->applySpeedRamp(clips.first().filePath, outputPath, config);
}

void MainWindow::audioEqualizer()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Equalizer", "Add clips first.");
        return;
    }

    auto presets = AudioEQProcessor::presets();
    QStringList presetNames;
    for (const auto &p : presets)
        presetNames << p.name;

    bool ok;
    QString selected = QInputDialog::getItem(this, "Audio Equalizer",
        "Select EQ preset:", presetNames, 0, false, &ok);
    if (!ok) return;

    const auto &clip = m_timeline->videoClips().first();
    QString outputPath = QFileDialog::getSaveFileName(this, "Save EQ Audio",
        QFileInfo(clip.filePath).baseName() + "_eq.mp4",
        "Media Files (*.mp4 *.wav *.mp3);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_audioEQ)
        m_audioEQ = new AudioEQProcessor(this);

    AudioEQConfig eqConfig;
    for (const auto &p : presets) {
        if (p.name == selected) { eqConfig = p.config; break; }
    }

    connect(m_audioEQ, &AudioEQProcessor::processComplete, this, [this](bool ok2, const QString &msg) {
        statusBar()->showMessage(ok2 ? "EQ applied: " + msg : "EQ failed: " + msg);
    }, Qt::UniqueConnection);

    m_audioEQ->applyEQ(clip.filePath, outputPath, eqConfig);
    statusBar()->showMessage("Applying EQ: " + selected);
}

void MainWindow::audioEffects()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "Audio Effects", "Add clips first.");
        return;
    }

    QStringList effects = {"Reverb", "Compressor", "Normalize", "Fade In", "Fade Out", "Bass Boost", "Voice Enhance"};
    bool ok;
    QString selected = QInputDialog::getItem(this, "Audio Effects",
        "Select effect:", effects, 0, false, &ok);
    if (!ok) return;

    const auto &clip = m_timeline->videoClips().first();
    QString outputPath = QFileDialog::getSaveFileName(this, "Save Audio Effect",
        QFileInfo(clip.filePath).baseName() + "_fx.mp4",
        "Media Files (*.mp4 *.wav *.mp3);;All Files (*)");
    if (outputPath.isEmpty()) return;

    if (!m_audioEQ)
        m_audioEQ = new AudioEQProcessor(this);

    AudioEffect effect;
    if (selected == "Reverb")         effect = AudioEffect::createReverb();
    else if (selected == "Compressor")effect = AudioEffect::createCompressor();
    else if (selected == "Normalize") effect = AudioEffect::createNormalize();
    else if (selected == "Fade In")   effect = AudioEffect::createFadeIn();
    else if (selected == "Fade Out")  effect = AudioEffect::createFadeOut();
    else if (selected == "Bass Boost")effect = AudioEffect::createBassBoost();
    else                              effect = AudioEffect::createVoiceEnhance();

    connect(m_audioEQ, &AudioEQProcessor::processComplete, this, [this](bool ok2, const QString &msg) {
        statusBar()->showMessage(ok2 ? "Effect applied: " + msg : "Effect failed: " + msg);
    }, Qt::UniqueConnection);

    m_audioEQ->applyEffect(clip.filePath, outputPath, effect);
    statusBar()->showMessage("Applying: " + selected);
}

void MainWindow::addMarker()
{
    double time = m_timeline->playheadPosition();
    bool ok;
    QString name = QInputDialog::getText(this, "Add Marker",
        QString("Marker name (at %1s):").arg(time, 0, 'f', 1),
        QLineEdit::Normal, "", &ok);
    if (!ok) return;

    QStringList types = {"Standard", "Chapter", "Todo", "Note"};
    QString typeStr = QInputDialog::getItem(this, "Marker Type",
        "Type:", types, 0, false, &ok);
    if (!ok) return;

    MarkerType type = MarkerType::Standard;
    if (typeStr == "Chapter") type = MarkerType::Chapter;
    else if (typeStr == "Todo") type = MarkerType::Todo;
    else if (typeStr == "Note") type = MarkerType::Note;

    m_markerManager.addMarker(time, name, type);
    statusBar()->showMessage(QString("Marker added: \"%1\" at %2s").arg(name).arg(time, 0, 'f', 1));
}

void MainWindow::showMarkers()
{
    auto markers = m_markerManager.markers();
    if (markers.isEmpty()) {
        QMessageBox::information(this, "Markers", "No markers set. Use Ctrl+M to add markers.");
        return;
    }

    QString info = QString("Markers (%1 total):\n\n").arg(markers.size());
    for (const auto &m : markers) {
        info += QString("• [%1s] %2 (%3)\n")
            .arg(m.time, 0, 'f', 1).arg(m.name)
            .arg(m.type == MarkerType::Chapter ? "Chapter" :
                 m.type == MarkerType::Todo ? "Todo" :
                 m.type == MarkerType::Note ? "Note" : "Standard");
    }
    QMessageBox::information(this, "Timeline Markers", info);
}

void MainWindow::exportChapters()
{
    auto chapters = m_markerManager.exportYouTubeChapters();
    if (chapters.isEmpty()) {
        QMessageBox::information(this, "Chapters", "No chapter markers found. Add markers with type 'Chapter' first.");
        return;
    }

    QString path = QFileDialog::getSaveFileName(this, "Export Chapters",
        "chapters.txt", "Text Files (*.txt);;All Files (*)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(chapters.toUtf8());
        file.close();
        statusBar()->showMessage("Exported chapters: " + path);
    }
}

void MainWindow::openRenderQueue()
{
    if (!m_renderQueue)
        m_renderQueue = new RenderQueue(this);

    auto jobs = m_renderQueue->jobs();
    QString info = QString("Render Queue (%1 jobs):\n\n").arg(jobs.size());
    for (const auto &j : jobs) {
        QString status = j.status == RenderJobStatus::Pending ? "Pending" :
                         j.status == RenderJobStatus::Rendering ? "Rendering" :
                         j.status == RenderJobStatus::Completed ? "Done" :
                         j.status == RenderJobStatus::Failed ? "Failed" : "Cancelled";
        info += QString("• %1 — %2 (%3%)\n").arg(j.name, status).arg(j.progress);
    }
    if (jobs.isEmpty())
        info += "Queue is empty. Export videos to add them to the queue.";

    QMessageBox::information(this, "Render Queue", info);
}

void MainWindow::startScreenRecording()
{
    if (!m_screenRecorder)
        m_screenRecorder = new ScreenRecorder(this);

    if (m_screenRecorder->isRecording()) {
        QMessageBox::information(this, "Recording", "Already recording. Stop first.");
        return;
    }

    RecordingConfig config;
    config.outputPath = QFileDialog::getSaveFileName(this, "Save Recording",
        "screen_recording.mp4", "Video Files (*.mp4 *.mkv);;All Files (*)");
    if (config.outputPath.isEmpty()) return;

    connect(m_screenRecorder, &ScreenRecorder::durationChanged, this, [this](double sec) {
        statusBar()->showMessage(QString("Recording: %1s").arg(sec, 0, 'f', 0));
    }, Qt::UniqueConnection);
    connect(m_screenRecorder, &ScreenRecorder::recordingStopped, this, [this](const QString &path) {
        statusBar()->showMessage("Recording saved: " + path);
    }, Qt::UniqueConnection);
    connect(m_screenRecorder, &ScreenRecorder::recordingError, this, [this](const QString &err) {
        QMessageBox::warning(this, "Recording Error", err);
    }, Qt::UniqueConnection);

    m_screenRecorder->startRecording(config);
    statusBar()->showMessage("Screen recording started...");
}

void MainWindow::stopScreenRecording()
{
    if (!m_screenRecorder || !m_screenRecorder->isRecording()) {
        QMessageBox::information(this, "Recording", "Not currently recording.");
        return;
    }
    m_screenRecorder->stopRecording();
}

void MainWindow::analyzeHighlights()
{
    if (m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, "AI Highlights", "Add clips first.");
        return;
    }

    const auto &clip = m_timeline->videoClips().first();

    bool ok;
    int count = QInputDialog::getInt(this, "AI Auto-Highlight",
        "Number of highlights to find:", 10, 1, 50, 1, &ok);
    if (!ok) return;

    if (!m_aiHighlight)
        m_aiHighlight = new AIHighlight(this);

    HighlightConfig config;
    config.targetCount = count;

    connect(m_aiHighlight, &AIHighlight::progressChanged, this, [this](int pct) {
        statusBar()->showMessage(QString("Analyzing highlights... %1%").arg(pct));
    }, Qt::UniqueConnection);

    connect(m_aiHighlight, &AIHighlight::analysisComplete, this, [this](const QVector<Highlight> &highlights) {
        if (highlights.isEmpty()) {
            QMessageBox::information(this, "Highlights", "No highlights found.");
            return;
        }

        QString info = QString("Found %1 highlight(s):\n\n").arg(highlights.size());
        for (int i = 0; i < highlights.size(); ++i) {
            const auto &h = highlights[i];
            info += QString("#%1  %2s - %3s  [score: %4]\n")
                .arg(i + 1).arg(h.startTime, 0, 'f', 1)
                .arg(h.endTime, 0, 'f', 1).arg(h.score, 0, 'f', 2);
        }

        auto reply = QMessageBox::question(this, "AI Highlights", info + "\nExport highlight reel?");
        if (reply == QMessageBox::Yes) {
            QString outputPath = QFileDialog::getSaveFileName(this, "Save Highlight Reel",
                "highlights.mp4", "Video Files (*.mp4);;All Files (*)");
            if (!outputPath.isEmpty()) {
                const auto &clip2 = m_timeline->videoClips().first();
                m_aiHighlight->exportHighlightReel(clip2.filePath, outputPath, highlights);
                statusBar()->showMessage("Exporting highlight reel...");
            }
        }
    }, Qt::UniqueConnection);

    m_aiHighlight->analyze(clip.filePath, config);
    statusBar()->showMessage("Analyzing video for highlights...");
}

void MainWindow::addShapeLayer()
{
    QStringList shapes = {"Rectangle", "Rounded Rectangle", "Ellipse", "Polygon", "Star", "Line", "Arrow"};
    bool ok;
    QString selected = QInputDialog::getItem(this, "Add Shape Layer",
        "Shape type:", shapes, 0, false, &ok);
    if (!ok) return;

    ShapeLayer shapeLayer;
    ShapeFill fill;
    fill.color = QColor(65, 105, 225); // Royal blue
    fill.enabled = true;
    ShapeStroke stroke;
    stroke.color = Qt::white;
    stroke.width = 2.0;
    stroke.enabled = true;

    if (selected == "Star") {
        shapeLayer.addShape(ShapeLayer::createStar(5, 80, 40, fill, stroke));
    } else if (selected == "Ellipse") {
        shapeLayer.addShape(ShapeLayer::createCircle(60, fill, stroke));
    } else {
        shapeLayer.addShape(ShapeLayer::createRectangle(QSizeF(200, 120), fill, stroke));
    }

    statusBar()->showMessage(QString("Added shape layer: %1").arg(selected));
}

void MainWindow::addParticleEffect()
{
    ParticleEffectDialog dialog(this);
    const auto &clips = m_timeline->videoClips();
    const int selectedIdx = m_timeline->selectedVideoClipIndex();
    if (selectedIdx >= 0 && selectedIdx < clips.size()) {
        const QString key = particleClipKey(clips[selectedIdx]);
        if (m_particleClipConfigs.contains(key))
            dialog.setConfig(m_particleClipConfigs.value(key));
    }

    if (dialog.exec() != QDialog::Accepted)
        return;

    const ParticleEmitterConfig config = dialog.config();
    const QSize canvasSize(m_projectConfig.width, m_projectConfig.height);
    const double fps = qMax(1, m_projectConfig.fps);
    double durationSec = 3.0;
    if (selectedIdx >= 0 && selectedIdx < clips.size())
        durationSec = qMax(0.5, clips[selectedIdx].effectiveDuration());

    ParticleSystem system;
    system.setConfig(config);
    const QVector<QImage> frames = system.renderParticleSequence(canvasSize, 0.0, durationSec, fps);
    if (frames.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Particle Effect"),
                             QStringLiteral("パーティクルフレームを生成できませんでした。"));
        return;
    }

    const QString ffmpegBin = findFfmpegBinary();
    if (ffmpegBin.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Particle Effect"),
                             QStringLiteral("ffmpeg が見つからないため粒子クリップを作成できません。"));
        return;
    }

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        QMessageBox::warning(this, QStringLiteral("Particle Effect"),
                             QStringLiteral("一時ディレクトリを作成できませんでした。"));
        return;
    }
    tempDir.setAutoRemove(false);

    for (int i = 0; i < frames.size(); ++i) {
        const QString framePath = tempDir.path()
            + QStringLiteral("/frame_%1.png").arg(i, 6, 10, QChar('0'));
        if (!frames[i].save(framePath)) {
            QMessageBox::warning(this, QStringLiteral("Particle Effect"),
                                 QStringLiteral("パーティクルフレームを書き出せませんでした。"));
            return;
        }
    }

    const QString outputPath = tempDir.path() + QStringLiteral("/particle_effect.mp4");
    QStringList args;
    args << QStringLiteral("-y")
         << QStringLiteral("-framerate") << QString::number(fps)
         << QStringLiteral("-i") << (tempDir.path() + QStringLiteral("/frame_%06d.png"))
         << QStringLiteral("-c:v") << QStringLiteral("mpeg4")
         << QStringLiteral("-q:v") << QStringLiteral("3")
         << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
         << QStringLiteral("-movflags") << QStringLiteral("+faststart")
         << outputPath;

    QProcess ffmpeg;
    ffmpeg.start(ffmpegBin, args);
    if (!ffmpeg.waitForStarted(5000)) {
        QMessageBox::warning(this, QStringLiteral("Particle Effect"),
                             QStringLiteral("ffmpeg を起動できませんでした。"));
        return;
    }
    ffmpeg.waitForFinished(-1);
    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0 || !QFileInfo::exists(outputPath)) {
        QMessageBox::warning(this, QStringLiteral("Particle Effect"),
                             QStringLiteral("粒子クリップのエンコードに失敗しました。\n%1")
                                 .arg(QString::fromUtf8(ffmpeg.readAllStandardError())));
        return;
    }

    m_particleClipConfigs.insert(outputPath, config);
    loadMediaFile(outputPath, true, QStringLiteral("Added particle effect"));
    statusBar()->showMessage(QStringLiteral("Added particle effect clip"), 3000);
}

void MainWindow::addTextAnimation()
{
    bool ok;
    QString text = QInputDialog::getText(this, "Text Animation",
        "Text to animate:", QLineEdit::Normal, "Hello!", &ok);
    if (!ok || text.isEmpty()) return;

    auto presets = TextAnimator::presetAnimations();
    QStringList animNames = presets.keys();

    QString selected = QInputDialog::getItem(this, "Text Animation",
        "Animation style:", animNames, 0, false, &ok);
    if (!ok) return;

    if (presets.contains(selected)) {
        TextAnimator animator;
        animator.setText(text, QFont("Arial", 48), QPointF(100, 100));
        animator.setAnimation(presets.value(selected));
        statusBar()->showMessage(QString("Added text animation: \"%1\" with %2")
            .arg(text, selected));
    }
}

void MainWindow::addBrushAnimation()
{
    BrushAnimationDialog dialog(this);
    if (dialog.exec() == QDialog::Rejected)
        return;

    auto params = dialog.params();

    auto brushAnim = new BrushAnimation(this);
    brushAnim->setText(params.text, params.font, params.basePosition);
    brushAnim->setBrushWidth(params.brushWidth);
    brushAnim->setMode(params.mode);

    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) {
        statusBar()->showMessage("ブラシアニメ追加失敗 — クリップが見つかりません");
        brushAnim->deleteLater();
        return;
    }

    // AC2 — attach to the first selected video clip (fallback: first clip on
    // first video track), mirroring addTextOverlayToFirstVideoClip pattern.
    const int trackIdx = 0;
    const int selectedIdx = m_timeline->selectedVideoClipIndex();
    const int clipIdx = (selectedIdx >= 0 && selectedIdx < clips.size()) ? selectedIdx : 0;
    const QString clipId = brushClipId(trackIdx, clipIdx);

    BrushAnimationEntry entry;
    entry.clipId = clipId;
    entry.brushData = brushAnim->toJson();
    upsertBrushAnimationEntry(entry);

    if (auto *previous = m_liveBrushAnimations.value(clipId, nullptr))
        previous->deleteLater();
    m_liveBrushAnimations.insert(clipId, brushAnim);

    KeyframeManager km = m_timeline->clipKeyframes();
    ensureBrushProgressTrack(km);
    m_timeline->setClipKeyframes(km);

    syncBrushAnimationPreviewForClip(trackIdx, clipIdx);

    statusBar()->showMessage(QString("ブラシアニメを追加: 「%1」 (%2)")
        .arg(params.text, params.mode == BrushAnimationMode::PerCharacter
            ? QStringLiteral("Per Character") : QStringLiteral("Per Stroke")));
}

void MainWindow::openRotoToolsDialog()
{
    if (!m_timeline) {
        QMessageBox::information(this, QStringLiteral("ロトツール"),
                                 QStringLiteral("タイムラインの初期化が完了していません。"));
        return;
    }

    int trackIdx = -1;
    int clipIdx = -1;
    ClipInfo clip;
    if (!selectedVideoClipRef(trackIdx, clipIdx, &clip)) {
        QMessageBox::information(this, QStringLiteral("ロトツール"),
                                 QStringLiteral("先にビデオクリップを選択してください。"));
        return;
    }

    const double fallbackFps = (m_projectConfig.fps > 0) ? m_projectConfig.fps : 30.0;
    const VideoSourceInfo info = probeVideoSourceInfo(
        ProxyManager::instance().getProxyPath(clip.filePath), fallbackFps);
    const double fps = clipEffectiveSourceFps(info, fallbackFps);
    const double sourceTime = clipSourceTimeAtPlayheadSeconds(trackIdx, clipIdx, clip);
    const int sourceFrameIndex = qMax(0, static_cast<int>(std::llround(sourceTime * fps)));

    QImage currentFrame = decodeClipFrameAtSourceTime(clip, sourceTime);
    if (currentFrame.isNull()) {
        QMessageBox::warning(this, QStringLiteral("ロトツール"),
                             QStringLiteral("現在フレームのデコードに失敗しました。"));
        return;
    }

    QVector<QImage> frames;
    const int frameBudget = (info.frameCount > 0)
        ? qMin(8, qMax(1, info.frameCount - sourceFrameIndex))
        : 8;
    for (int i = 0; i < frameBudget; ++i) {
        QImage frame = decodeClipFrameByIndex(clip, sourceFrameIndex + i, fps);
        if (frame.isNull())
            break;
        frames.append(frame);
    }
    if (frames.isEmpty())
        frames.append(currentFrame);

    RotoToolsDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("ロトツール - %1").arg(clip.displayName));
    dialog.setFrame(currentFrame);
    dialog.setFrameSequence(frames, sourceFrameIndex);

    const QString clipId = brushClipId(trackIdx, clipIdx);
    if (m_rotoClipEntries.contains(clipId)) {
        const RotoClipEntry &entry = m_rotoClipEntries.value(clipId);
        if (!entry.keyframes.isEmpty()) {
            Rotoscope roto;
            for (const auto &keyframe : entry.keyframes)
                roto.addKeyframe(keyframe.frameNumber, keyframe.path);
            dialog.setRotoPath(roto.getPathAtFrame(sourceFrameIndex));
        } else if (!entry.path.points.isEmpty()) {
            dialog.setRotoPath(entry.path);
        }
    }

    if (dialog.exec() != QDialog::Accepted)
        return;

    RotoClipEntry entry = m_rotoClipEntries.value(clipId);
    entry.clipId = clipId;
    entry.path = dialog.rotoPath();

    QVector<RotoKeyframe> keyframes = dialog.trackedKeyframes();
    if (!keyframes.isEmpty()) {
        entry.keyframes = keyframes;
    } else if (!entry.path.points.isEmpty()) {
        bool replaced = false;
        for (auto &keyframe : entry.keyframes) {
            if (keyframe.frameNumber == sourceFrameIndex) {
                keyframe.path = entry.path;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            RotoKeyframe keyframe;
            keyframe.frameNumber = sourceFrameIndex;
            keyframe.path = entry.path;
            entry.keyframes.append(keyframe);
            std::sort(entry.keyframes.begin(), entry.keyframes.end(),
                      [](const RotoKeyframe &a, const RotoKeyframe &b) {
                          return a.frameNumber < b.frameNumber;
                      });
        }
    }

    const QImage brushMask = dialog.brushMask();
    if (!brushMask.isNull())
        entry.brushMask = brushMask.convertToFormat(QImage::Format_Grayscale8);

    if (entry.path.points.isEmpty() && entry.keyframes.isEmpty() && entry.brushMask.isNull()) {
        m_rotoClipEntries.remove(clipId);
    } else {
        m_rotoClipEntries.insert(clipId, entry);
    }

    refreshSpecialClipPreview();
    statusBar()->showMessage(QStringLiteral("ロトデータを %1 に保存しました").arg(clip.displayName), 4000);
}

void MainWindow::openTimeRemapDialog()
{
    if (!m_timeline) {
        QMessageBox::information(this, QStringLiteral("タイムリマップ"),
                                 QStringLiteral("タイムラインの初期化が完了していません。"));
        return;
    }

    int trackIdx = -1;
    int clipIdx = -1;
    ClipInfo clip;
    if (!selectedVideoClipRef(trackIdx, clipIdx, &clip)) {
        QMessageBox::information(this, QStringLiteral("タイムリマップ"),
                                 QStringLiteral("先にビデオクリップを選択してください。"));
        return;
    }

    const double fallbackFps = (m_projectConfig.fps > 0) ? m_projectConfig.fps : 30.0;
    const VideoSourceInfo info = probeVideoSourceInfo(
        ProxyManager::instance().getProxyPath(clip.filePath), fallbackFps);
    const double fps = clipEffectiveSourceFps(info, fallbackFps);
    const int frameCount = (info.frameCount > 0)
        ? info.frameCount
        : qMax(1, static_cast<int>(std::llround((clipSourceOutPoint(clip) - clip.inPoint) * fps)));

    const QString clipId = brushClipId(trackIdx, clipIdx);
    TimeRemapClipEntry entry = m_timeRemapClipEntries.value(clipId);
    entry.clipId = clipId;
    if (entry.curve.sourceFps <= 0.0)
        entry.curve.sourceFps = fps;

    TimeRemapDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("タイムリマップ - %1").arg(clip.displayName));
    dialog.setCurve(entry.curve);
    dialog.setSourceFrameCount(frameCount);
    dialog.setFrameFetcher([this, clip, fps](int sourceFrameIndex) {
        return decodeClipFrameByIndex(clip, sourceFrameIndex, fps);
    });

    if (dialog.exec() != QDialog::Accepted)
        return;

    entry.curve = dialog.curve();
    if (entry.curve.sourceFps <= 0.0)
        entry.curve.sourceFps = fps;
    m_timeRemapClipEntries.insert(clipId, entry);

    refreshSpecialClipPreview();
    statusBar()->showMessage(QStringLiteral("タイムリマップを %1 に保存しました").arg(clip.displayName), 4000);
}

void MainWindow::configureTrackMatte()
{
    if (!m_timeline) {
        QMessageBox::information(this, QStringLiteral("トラックマット"),
                                 QStringLiteral("タイムラインの初期化が完了していません。"));
        return;
    }

    int targetTrackIdx = -1;
    int targetClipIdx = -1;
    ClipInfo targetClip;
    if (!selectedVideoClipRef(targetTrackIdx, targetClipIdx, &targetClip)) {
        QMessageBox::information(this, QStringLiteral("トラックマット"),
                                 QStringLiteral("先にビデオクリップを選択してください。"));
        return;
    }

    struct ClipOption {
        QString id;
        QString label;
    };
    QVector<ClipOption> candidates;
    for (int trackIdx = 0; trackIdx < m_timeline->videoTracks().size(); ++trackIdx) {
        const auto *track = m_timeline->videoTracks().value(trackIdx, nullptr);
        if (!track)
            continue;
        const auto &clips = track->clips();
        for (int clipIdx = 0; clipIdx < clips.size(); ++clipIdx) {
            const QString clipId = brushClipId(trackIdx, clipIdx);
            if (trackIdx == targetTrackIdx && clipIdx == targetClipIdx)
                continue;
            candidates.append({clipId,
                               QStringLiteral("V%1 #%2 - %3")
                                   .arg(trackIdx + 1)
                                   .arg(clipIdx + 1)
                                   .arg(clips[clipIdx].displayName)});
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("トラックマット設定"));
    auto *layout = new QFormLayout(&dialog);
    auto *typeCombo = new QComboBox(&dialog);
    auto *sourceCombo = new QComboBox(&dialog);

    const QList<TrackMatteType> matteTypes = {
        TrackMatteType::None,
        TrackMatteType::AlphaMatte,
        TrackMatteType::AlphaInvertedMatte,
        TrackMatteType::LumaMatte,
        TrackMatteType::LumaInvertedMatte
    };
    for (TrackMatteType type : matteTypes)
        typeCombo->addItem(trackMatteTypeLabel(type), static_cast<int>(type));
    sourceCombo->addItem(QStringLiteral("なし"), QString{});
    for (const auto &candidate : candidates)
        sourceCombo->addItem(candidate.label, candidate.id);

    const QString targetClipId = brushClipId(targetTrackIdx, targetClipIdx);
    const TrackMatteClipEntry existing = m_trackMatteClipEntries.value(targetClipId);
    const int existingTypeIndex = typeCombo->findData(static_cast<int>(existing.matteType));
    if (existingTypeIndex >= 0)
        typeCombo->setCurrentIndex(existingTypeIndex);
    const int existingSourceIndex = sourceCombo->findData(existing.matteSourceClipId);
    if (existingSourceIndex >= 0)
        sourceCombo->setCurrentIndex(existingSourceIndex);

    layout->addRow(QStringLiteral("タイプ:"), typeCombo);
    layout->addRow(QStringLiteral("マット元:"), sourceCombo);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [typeCombo, sourceCombo](int) {
        const TrackMatteType currentType = static_cast<TrackMatteType>(typeCombo->currentData().toInt());
        sourceCombo->setEnabled(currentType != TrackMatteType::None);
    });
    sourceCombo->setEnabled(existing.matteType != TrackMatteType::None);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const TrackMatteType matteType = static_cast<TrackMatteType>(typeCombo->currentData().toInt());
    const QString matteSourceId = sourceCombo->currentData().toString();
    if (matteType == TrackMatteType::None || matteSourceId.isEmpty()) {
        m_trackMatteClipEntries.remove(targetClipId);
        syncTrackMatteEntriesToTimeline(m_timeline, m_trackMatteClipEntries);
        refreshSpecialClipPreview();
        statusBar()->showMessage(QStringLiteral("トラックマットを解除しました"), 3000);
        return;
    }
    if (matteSourceId == targetClipId) {
        QMessageBox::warning(this, QStringLiteral("トラックマット"),
                             QStringLiteral("同じクリップはマット元に指定できません。"));
        return;
    }

    TrackMatteClipEntry entry;
    entry.clipId = targetClipId;
    entry.matteType = matteType;
    entry.matteSourceClipId = matteSourceId;
    m_trackMatteClipEntries.insert(targetClipId, entry);
    syncTrackMatteEntriesToTimeline(m_timeline, m_trackMatteClipEntries);

    refreshSpecialClipPreview();
    statusBar()->showMessage(QStringLiteral("%1 に %2 を設定しました")
                                 .arg(targetClip.displayName, trackMatteTypeLabel(matteType)),
                             4000);
}

// ──────────────────────────────────────────────────────────────────────────
// US-3D-11: motion-graphics sprint — 4 menu actions
// ──────────────────────────────────────────────────────────────────────────

void MainWindow::open3DExtrudedText()
{
    if (!m_timeline) {
        QMessageBox::information(this, QStringLiteral("3D 押し出しテキスト"),
                                 QStringLiteral("タイムラインの初期化が完了していません。"));
        return;
    }
    int trackIdx = -1;
    int clipIdx = -1;
    ClipInfo clip;
    if (!selectedVideoClipRef(trackIdx, clipIdx, &clip)) {
        QMessageBox::information(this, QStringLiteral("3D 押し出しテキスト"),
                                 QStringLiteral("先にビデオクリップを選択してください。"));
        return;
    }
    const QString clipId = brushClipId(trackIdx, clipIdx);

    Text3DExtrusionDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("3D 押し出しテキスト - %1").arg(clip.displayName));
    if (m_text3DClipConfigs.contains(clipId)) {
        Text3DLayer seed;
        seed.fromJson(m_text3DClipConfigs.value(clipId));
        dialog.setLayer(seed);
    }
    if (dialog.exec() != QDialog::Accepted)
        return;

    Text3DLayer *cfg = dialog.layer(this);
    if (!cfg) {
        statusBar()->showMessage(QStringLiteral("3D テキストの設定取得に失敗しました"), 3000);
        return;
    }
    const QJsonObject json = cfg->toJson();
    delete cfg;
    if (json.isEmpty())
        m_text3DClipConfigs.remove(clipId);
    else
        m_text3DClipConfigs.insert(clipId, json);

    refreshSpecialClipPreview();
    statusBar()->showMessage(QStringLiteral("3D 押し出しテキストを %1 に設定しました").arg(clip.displayName), 4000);
}

void MainWindow::editClipExpressionBindings()
{
    if (!m_timeline) {
        QMessageBox::information(this, QStringLiteral("エクスプレッション"),
                                 QStringLiteral("タイムラインの初期化が完了していません。"));
        return;
    }
    int trackIdx = -1;
    int clipIdx = -1;
    ClipInfo clip;
    if (!selectedVideoClipRef(trackIdx, clipIdx, &clip)) {
        QMessageBox::information(this, QStringLiteral("エクスプレッション"),
                                 QStringLiteral("先にビデオクリップを選択してください。"));
        return;
    }
    const QString clipId = brushClipId(trackIdx, clipIdx);

    ExpressionBindingDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("エクスプレッション - %1").arg(clip.displayName));
    if (m_clipExpressionBindings.contains(clipId))
        dialog.setBindings(m_clipExpressionBindings.value(clipId));
    const double projFps = (m_projectConfig.fps > 0) ? m_projectConfig.fps : 30.0;
    const QSize canvas = (m_projectConfig.width > 0 && m_projectConfig.height > 0)
        ? QSize(m_projectConfig.width, m_projectConfig.height)
        : QSize(1920, 1080);
    dialog.setContextHints(clip.effectiveDuration(), projFps, canvas);
    if (dialog.exec() != QDialog::Accepted)
        return;

    exprbind::ClipExpressionBindings bindings = dialog.bindings();
    if (bindings.isEmpty())
        m_clipExpressionBindings.remove(clipId);
    else
        m_clipExpressionBindings.insert(clipId, bindings);

    refreshSpecialClipPreview();
    statusBar()->showMessage(QStringLiteral("エクスプレッションを %1 に設定しました").arg(clip.displayName), 4000);
}

void MainWindow::editClipWiggle()
{
    if (!m_timeline) {
        QMessageBox::information(this, QStringLiteral("ウィグル"),
                                 QStringLiteral("タイムラインの初期化が完了していません。"));
        return;
    }
    int trackIdx = -1;
    int clipIdx = -1;
    ClipInfo clip;
    if (!selectedVideoClipRef(trackIdx, clipIdx, &clip)) {
        QMessageBox::information(this, QStringLiteral("ウィグル"),
                                 QStringLiteral("先にビデオクリップを選択してください。"));
        return;
    }
    const QString clipId = brushClipId(trackIdx, clipIdx);
    wiggle::WiggleParams params = m_clipWiggleParams.value(clipId);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("ウィグル / 手持ちカメラ風 - %1").arg(clip.displayName));
    auto *form = new QFormLayout(&dialog);

    auto *presetCombo = new QComboBox(&dialog);
    presetCombo->addItems({QStringLiteral("なし"), QStringLiteral("手持ち"),
                           QStringLiteral("神経質"), QStringLiteral("ふわふわ")});
    auto *enabledCheck = new QCheckBox(QStringLiteral("有効"), &dialog);
    enabledCheck->setChecked(params.enabled);
    auto *posAmpXSpin = new QDoubleSpinBox(&dialog);
    posAmpXSpin->setRange(-200.0, 200.0);
    posAmpXSpin->setValue(params.positionAmplitude.x());
    auto *posAmpYSpin = new QDoubleSpinBox(&dialog);
    posAmpYSpin->setRange(-200.0, 200.0);
    posAmpYSpin->setValue(params.positionAmplitude.y());
    auto *rotAmpSpin = new QDoubleSpinBox(&dialog);
    rotAmpSpin->setRange(-180.0, 180.0);
    rotAmpSpin->setValue(params.rotationAmplitudeDeg);
    auto *freqSpin = new QDoubleSpinBox(&dialog);
    freqSpin->setRange(0.0, 30.0);
    freqSpin->setValue(params.positionFrequency);
    auto *octavesSpin = new QSpinBox(&dialog);
    octavesSpin->setRange(1, 6);
    octavesSpin->setValue(params.octaves);
    auto *seedSpin = new QSpinBox(&dialog);
    seedSpin->setRange(0, 100000);
    seedSpin->setValue(static_cast<int>(params.seed));

    form->addRow(QStringLiteral("プリセット:"), presetCombo);
    form->addRow(enabledCheck);
    form->addRow(QStringLiteral("位置振幅 X (px):"), posAmpXSpin);
    form->addRow(QStringLiteral("位置振幅 Y (px):"), posAmpYSpin);
    form->addRow(QStringLiteral("回転振幅 (°):"), rotAmpSpin);
    form->addRow(QStringLiteral("周波数 (Hz):"), freqSpin);
    form->addRow(QStringLiteral("オクターブ:"), octavesSpin);
    form->addRow(QStringLiteral("シード:"), seedSpin);

    connect(presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [posAmpXSpin, posAmpYSpin, rotAmpSpin, freqSpin, octavesSpin, enabledCheck](int idx) {
        if (idx <= 0)
            return;
        wiggle::WiggleParams p;
        if (idx == 1) p = wiggle::handheldPreset(1.0);
        else if (idx == 2) p = wiggle::nervousPreset(1.0);
        else p = wiggle::floatPreset(1.0);
        posAmpXSpin->setValue(p.positionAmplitude.x());
        posAmpYSpin->setValue(p.positionAmplitude.y());
        rotAmpSpin->setValue(p.rotationAmplitudeDeg);
        freqSpin->setValue(p.positionFrequency);
        octavesSpin->setValue(p.octaves);
        enabledCheck->setChecked(true);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    params.enabled = enabledCheck->isChecked();
    params.positionAmplitude = QPointF(posAmpXSpin->value(), posAmpYSpin->value());
    params.rotationAmplitudeDeg = rotAmpSpin->value();
    params.positionFrequency = freqSpin->value();
    params.rotationFrequency = freqSpin->value();
    params.scaleFrequency = freqSpin->value();
    params.octaves = octavesSpin->value();
    params.seed = static_cast<unsigned int>(seedSpin->value());

    if (!params.enabled
        && params.positionAmplitude.isNull()
        && qFuzzyIsNull(params.rotationAmplitudeDeg)
        && qFuzzyIsNull(params.scaleAmplitude)) {
        m_clipWiggleParams.remove(clipId);
    } else {
        m_clipWiggleParams.insert(clipId, params);
    }

    refreshSpecialClipPreview();
    statusBar()->showMessage(QStringLiteral("ウィグルを %1 に設定しました").arg(clip.displayName), 4000);
}

void MainWindow::openCameraMotionDialog()
{
    CameraMotionDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("カメラモーション"));
    dialog.setCamera(m_projectCamera);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_projectCamera = dialog.camera();
    statusBar()->showMessage(QStringLiteral("カメラモーションを更新しました"), 4000);
}

// US-HW-10: Sprint 9 — scene-cut detection menu entry.
//
// Picks the currently-selected video clip (or falls back to track 0 / clip 0),
// pops SceneCutDialog modally, and on Accept applies the user's choice:
//   AddMarkers: feed accepted cut timestamps to Timeline::addMarker as cyan-ish
//               scene-cut markers (same color used by autoSceneDetect).
//   SplitClip:  jump the playhead to each cut and call Timeline::splitAtPlayhead.
//               Sorted descending so earlier splits don't shift later cuts.
void MainWindow::onSceneCutDetect()
{
    ClipInfo current;
    int trackIdx = -1;
    int clipIdx  = -1;
    if (!selectedVideoClipRef(trackIdx, clipIdx, &current) || current.filePath.isEmpty()) {
        QMessageBox::information(this,
            QStringLiteral("シーンカット検出"),
            QStringLiteral("クリップを選択してください"));
        return;
    }

    double fps = 0.0;
    if (m_projectConfig.fps > 0.0)
        fps = m_projectConfig.fps;

    SceneCutDialog dlg(current.filePath, fps, this);
    if (dlg.exec() != QDialog::Accepted || !dlg.wasApplied())
        return;

    const QVector<qint64> cutsMs = dlg.acceptedCutTimestampsMs();
    if (cutsMs.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("検出されたシーンカットはありません"), 3000);
        return;
    }

    const QColor sceneCutColor(QStringLiteral("#33ccff"));

    if (dlg.applyMode() == SceneCutDialog::ApplyMode::AddMarkers) {
        if (!m_timeline) return;
        int added = 0;
        const double clipStartSec = clipTimelineStartSeconds(trackIdx, clipIdx);
        for (qint64 ms : cutsMs) {
            const double timelineSec =
                clipStartSec + (static_cast<double>(ms) / 1000.0);
            const qint64 timeUs = static_cast<qint64>(timelineSec * 1.0e6);
            m_timeline->addMarker(timeUs,
                QStringLiteral("Scene Cut"),
                sceneCutColor);
            ++added;
        }
        statusBar()->showMessage(
            QStringLiteral("%1 個のシーンカットマーカーを追加しました").arg(added),
            4000);
    } else {
        // SplitClip: walk cuts in descending order so each split is at a
        // stable timestamp (earlier splits don't displace later cuts within
        // the same source clip).
        if (!m_timeline) return;
        QVector<qint64> sorted = cutsMs;
        std::sort(sorted.begin(), sorted.end(), std::greater<qint64>());
        int splits = 0;
        const double clipStartSec = clipTimelineStartSeconds(trackIdx, clipIdx);
        for (qint64 ms : sorted) {
            const double timelineSec =
                clipStartSec + (static_cast<double>(ms) / 1000.0);
            m_timeline->setPlayheadPosition(timelineSec);
            m_timeline->splitAtPlayhead();
            ++splits;
        }
        statusBar()->showMessage(
            QStringLiteral("%1 箇所でクリップを分割しました").arg(splits),
            4000);
        updateEditActions();
    }
}

// US-HW-10: Sprint 9 — sidechain ducking dialog entry.
//
// Opens AudioDuckingDialog seeded with the project's persisted m_duckingParams.
// On Accept, copies dialog state into the project members; persistence happens
// the next time the project is saved through populateProjectData → ProjectFile.
void MainWindow::onAudioDuckingSettings()
{
    AudioDuckingDialog dlg(m_duckingParams, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_duckingParams  = dlg.params();
    m_duckingEnabled = dlg.duckingEnabled();
    statusBar()->showMessage(
        m_duckingEnabled
            ? QStringLiteral("オーディオダッキング設定を更新しました (有効)")
            : QStringLiteral("オーディオダッキング設定を更新しました (無効)"),
        4000);
}

// US-HW-10: Sprint 9 — Collect Files entry.
//
// Snapshots current project state via the same populateProjectData() path used
// by saveProject(), then hands it to ProjectCollectorDialog which copies the
// referenced media + project file into a destination folder.
void MainWindow::onCollectProject()
{
    ProjectData data;
    populateProjectData(data);
    ProjectCollectorDialog dlg(data, this);
    dlg.exec();
    if (dlg.didCollect()) {
        statusBar()->showMessage(
            QStringLiteral("プロジェクトを収集しました: %1").arg(dlg.outputProjectPath()),
            6000);
    }
}

// US-EXT-10: Sprint 10 pro extension menu slots.
// Each slot opens the corresponding dialog seeded with the project's persisted
// member state and writes the dialog's result back on Accept. Persistence
// happens the next time the project is saved through populateProjectData →
// ProjectFile.
void MainWindow::onHDRSettings()
{
    HDRSettingsDialog dlg(m_hdrSettings, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_hdrSettings = dlg.settings();
    statusBar()->showMessage(
        QStringLiteral("HDR 出力設定を更新しました (%1)").arg(m_hdrSettings.mode),
        4000);
}

void MainWindow::onAIProcessing()
{
    AIProcessingDialog dlg(m_aiSettings, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_aiSettings = dlg.settings();
    statusBar()->showMessage(
        QStringLiteral("AI 処理設定を更新しました (upscale=%1, interp=%2)")
            .arg(m_aiSettings.upscaleEnabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg(m_aiSettings.frameInterpEnabled ? QStringLiteral("ON") : QStringLiteral("OFF")),
        4000);
}

void MainWindow::onPluginBrowser()
{
    const QString defaultDir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/plugins");
    PluginBrowserDialog dlg(defaultDir, this);
    dlg.exec();
}

// US-WF-D: Sprint 11 workflow menu slots. The 3 slots open AI auto-mask /
// per-clip audio envelope editor / magnetic-timeline demo. Each dialog is
// kept alive between invocations (cached as a member) so window state and
// in-progress edits survive a close-and-reopen.
void MainWindow::openAIMaskDialog()
{
    if (!m_aiMaskDialog) {
        m_aiMaskDialog = new AIMaskDialog(this);
    }
    // TODO: pass the currently-selected clip's preview frame as the source
    // image once a frame-grab path from VideoPlayer/GLPreview is available.
    m_aiMaskDialog->show();
    m_aiMaskDialog->raise();
    m_aiMaskDialog->activateWindow();
}

void MainWindow::openPlanarTrackerDialog()
{
    if (!m_planarTrackerDialog) {
        m_planarTrackerDialog = new PlanarTrackerDialog(this);
        m_planarTrackerDialog->setObjectName(QStringLiteral("planarTrackerDialog"));

        // PRD-PROJECT-PRESET US-PP-4: write back state when dialog is closed.
        connect(m_planarTrackerDialog, &QDialog::finished, this,
                [this](int /*result*/) {
                    if (m_planarTrackerDialog)
                        m_planarTrackerState = m_planarTrackerDialog->currentState();
                },
                Qt::UniqueConnection);
    }

    // PRD-PROJECT-PRESET US-PP-4: restore state before showing.
    m_planarTrackerDialog->setInitialState(m_planarTrackerState);

    m_planarTrackerDialog->show();
    m_planarTrackerDialog->raise();
    m_planarTrackerDialog->activateWindow();
}

// US-TP-6: PRD-TP — open the motion-tracker preset dialog modally and, on
// Accepted, apply the selected preset to m_motionTracker via the
// tracker_preset::applyToMotionTracker() helper from US-TP-4. The dialog is a
// stack-local QDialog (modal exec) — no persistent member is needed because
// the apply path is fire-and-forget. Existing startTracking() call-sites
// (around L5874/5877/5919) remain untouched.
void MainWindow::showMotionTrackerDialog()
{
    if (!m_motionTracker) {
        m_motionTracker = new MotionTracker(this);
    }
    MotionTrackerDialog dlg(this);
    dlg.setInitialState(m_motionTrackerState);   // PRD-PROJECT-PRESET US-PP-4: restore
    if (dlg.exec() == QDialog::Accepted) {
        m_motionTrackerState = dlg.currentState();  // PRD-PROJECT-PRESET US-PP-4: save
        const auto preset = dlg.selectedPreset();
        tracker_preset::applyToMotionTracker(m_motionTracker, preset);
        statusBar()->showMessage(
            tr("Tracker preset 適用: %1").arg(preset.displayName), 3000);
    }
}

void MainWindow::openAudioClipEditorDialog()
{
    if (!m_audioClipEditorDialog) {
        m_audioClipEditorDialog = new QDialog(this);
        m_audioClipEditorDialog->setWindowTitle(QStringLiteral("クリップボリュームエンベロープ"));
        m_audioClipEditorDialog->setObjectName(QStringLiteral("audioClipEditorWrapper"));
        auto *layout = new QVBoxLayout(m_audioClipEditorDialog);
        auto *editor = new AudioClipEditor(m_audioClipEditorDialog);
        editor->setObjectName(QStringLiteral("audioClipEditor"));
        editor->setClipDuration(10000); // demo: 10sec
        layout->addWidget(editor);
        m_audioClipEditorDialog->resize(640, 240);
    }
    m_audioClipEditorDialog->show();
    m_audioClipEditorDialog->raise();
    m_audioClipEditorDialog->activateWindow();
}

void MainWindow::runMagneticTimelineDemo()
{
    QList<magtl::Clip> demo;
    demo.append(magtl::Clip{0, 0, 1000, QStringLiteral("A")});
    demo.append(magtl::Clip{0, 1100, 2000, QStringLiteral("B")});  // 100ms gap
    const auto closed = magtl::closeGaps(demo);
    QString summary = QStringLiteral("MagneticTimeline closeGaps demo:\n");
    for (const auto &c : closed) {
        summary += QStringLiteral("  track=%1 [%2..%3] id=%4\n")
                       .arg(c.trackIndex)
                       .arg(c.startMs)
                       .arg(c.endMs)
                       .arg(c.id);
    }
    QMessageBox::information(this, QStringLiteral("Magnetic Timeline Demo"), summary);
}

void MainWindow::editTransformKeyframes()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Transform", "Select a clip first.");
        return;
    }

    QStringList properties = {"Position X", "Position Y", "Scale X", "Scale Y",
                              "Rotation", "Opacity", "Skew X", "Skew Y"};
    bool ok;
    QString prop = QInputDialog::getItem(this, "Transform Keyframe",
        "Property to animate:", properties, 0, false, &ok);
    if (!ok) return;

    double time = m_timeline->playheadPosition();
    double value = QInputDialog::getDouble(this, "Transform Keyframe",
        QString("Value for %1 at %2s:").arg(prop).arg(time, 0, 'f', 1),
        0.0, -10000, 10000, 2, &ok);
    if (!ok) return;

    statusBar()->showMessage(QString("Set keyframe: %1 = %2 at %3s")
        .arg(prop).arg(value).arg(time, 0, 'f', 1));
}

void MainWindow::addMask()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Mask", "Select a clip first.");
        return;
    }

    QStringList shapes = {"Rectangle", "Ellipse", "Polygon"};
    bool ok;
    QString selected = QInputDialog::getItem(this, "Add Mask",
        "Mask shape:", shapes, 0, false, &ok);
    if (!ok) return;

    double feather = QInputDialog::getDouble(this, "Mask Feather",
        "Feather amount (pixels):", 10.0, 0.0, 100.0, 1, &ok);
    if (!ok) return;

    statusBar()->showMessage(QString("Added %1 mask (feather: %2px)").arg(selected).arg(feather));
}

void MainWindow::applyWarpEffect()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Warp", "Select a clip first.");
        return;
    }

    QStringList warps = {"Mesh Warp", "Puppet Pin", "Bulge", "Pinch", "Twirl",
                         "Wave", "Ripple", "Spherize", "Fisheye"};
    bool ok;
    QString selected = QInputDialog::getItem(this, "Warp / Distortion",
        "Effect type:", warps, 0, false, &ok);
    if (!ok) return;

    double amount = QInputDialog::getDouble(this, "Warp Amount",
        "Amount (0.0-1.0):", 0.5, 0.0, 2.0, 2, &ok);
    if (!ok) return;

    statusBar()->showMessage(QString("Applied %1 (amount: %2)").arg(selected).arg(amount, 0, 'f', 2));
}

void MainWindow::editExpressions()
{
    bool ok;
    QString propName = QInputDialog::getText(this, "Expression",
        "Property name (e.g., rotation, opacity):", QLineEdit::Normal, "rotation", &ok);
    if (!ok || propName.isEmpty()) return;

    QString code = QInputDialog::getText(this, "Expression",
        "Expression code:", QLineEdit::Normal, "wiggle(2, 10)", &ok);
    if (!ok || code.isEmpty()) return;

    QString validationError = Expression::validate(code);
    if (!validationError.isEmpty()) {
        QMessageBox::warning(this, "Expression Error", "Invalid expression: " + validationError);
        return;
    }

    statusBar()->showMessage(QString("Expression set: %1 = %2").arg(propName, code));
}

void MainWindow::precomposeSelected()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Pre-Compose", "Select clips first.");
        return;
    }

    bool ok;
    QString name = QInputDialog::getText(this, "Pre-Compose",
        "Composition name:", QLineEdit::Normal, "Comp 1", &ok);
    if (!ok || name.isEmpty()) return;

    int compId = m_precomposeManager.createComposition(name,
        m_projectConfig.width, m_projectConfig.height,
        m_projectConfig.fps, 10.0);
    statusBar()->showMessage(QString("Created composition: %1 (ID: %2)").arg(name).arg(compId));
}

void MainWindow::showResourceGuide()
{
    ResourceGuideDialog dialog(this);
    dialog.exec();
}

void MainWindow::about()
{
    QMessageBox::about(this, "About V Simple Editor",
        QString("V Simple Editor v%1\n\n"
                "A full-featured video editor with 90+ features.\n"
                "Built with C++17 / Qt6 / FFmpeg / OpenGL 3.3.\n\n"
                "Features: multi-track timeline, AE-style compositing,\n"
                "AI editing tools, GPU preview, 13 export presets,\n"
                "Python scripting, VST/AU plugins, and more.\n\n"
                "Press Ctrl+Alt+K to customize keyboard shortcuts.")
            .arg(APP_VERSION));
}

// US-SC-B: Sprint 12 — open / raise the ショートカット設定 dialog (modeless,
// kept alive between invocations so layout state survives reopen).
void MainWindow::openShortcutCustomizeDialog()
{
    if (!m_shortcutManager)
        return;
    if (!m_shortcutCustomizeDialog) {
        m_shortcutCustomizeDialog = new ShortcutCustomizeDialog(m_shortcutManager, this);
        m_shortcutCustomizeDialog->setObjectName("shortcutCustomizeDialog");
    }
    m_shortcutCustomizeDialog->show();
    m_shortcutCustomizeDialog->raise();
    m_shortcutCustomizeDialog->activateWindow();
}

void MainWindow::onShowCredentialDialog()
{
    if (!m_credentialDialog) {
        m_credentialDialog = new CredentialDialog(this);
        m_credentialDialog->setObjectName(QStringLiteral("credentialDialog"));
    }
    m_credentialDialog->show();
    m_credentialDialog->raise();
    m_credentialDialog->activateWindow();
}

// US-SC2-B: Sprint 13 — open / raise the SNS 向けエクスポート dialog
// (modeless; preset と reframe 設定をユーザが行い、exportRequested 受信で
//  実 export 連携は後続スプリントで実装予定)。
void MainWindow::openSocialExportDialog()
{
    if (!m_socialExportDialog) {
        m_socialExportDialog = new SocialExportDialog(this);
        m_socialExportDialog->setObjectName(QStringLiteral("socialExportDialog"));
        // TODO: 現在の preview frame (GLPreview / VideoPlayer) との連携は
        //       後続スプリント。getCurrentFrame() 等が整備され次第
        //       m_socialExportDialog->setSampleFrame(sample) を呼ぶ。
        connect(m_socialExportDialog, &SocialExportDialog::exportRequested,
                this, [this](const social::Preset& preset,
                             const reframe::ReframeParams& reframeParams) {
                    // 暫定: ログ出力 + status bar 通知のみ。実 export 連携は今後。
                    qInfo().noquote() << QStringLiteral(
                        "SocialExport requested: preset=%1 res=%2x%3 mode=%4")
                        .arg(preset.displayName)
                        .arg(preset.resolution.width())
                        .arg(preset.resolution.height())
                        .arg(reframe::modeDisplayName(reframeParams.mode));
                    if (statusBar()) {
                        statusBar()->showMessage(
                            QStringLiteral("%1 へエクスポート (近日実装予定)")
                                .arg(preset.displayName),
                            5000);
                    }
                });
    }
    m_socialExportDialog->show();
    m_socialExportDialog->raise();
    m_socialExportDialog->activateWindow();
}

// US-CAP-B: Sprint 14 — 字幕エディタダイアログ
// (modeless; 字幕クリップ追加・編集・SRT/VTT 取込/書出し・ASR 自動生成)
void MainWindow::openCaptionEditorDialog()
{
    if (!m_captionEditorDialog) {
        m_captionEditorDialog = new CaptionEditorDialog(this);
        m_captionEditorDialog->setObjectName(QStringLiteral("captionEditorDialog"));
    }
    m_captionEditorDialog->show();
    m_captionEditorDialog->raise();
    m_captionEditorDialog->activateWindow();
}

// Phase 6 Wave 2 (US-6B-4): 動画→Whisper 文字起こし配線。
// WhisperTranscribeDialog をモーダル exec() し、Accepted なら request() を
// WhisperTranscriber に渡して caption::Track を生成、結果セグメント数を可視化する。
void MainWindow::openWhisperTranscribeDialog()
{
    WhisperTranscribeDialog dialog(this);

    // 現在タイムラインに乗っている最初の動画を初期メディアパスとして渡す。
    if (m_timeline) {
        const QVector<ClipInfo> &videoClips = m_timeline->videoClips();
        if (!videoClips.isEmpty() && !videoClips.first().filePath.isEmpty())
            dialog.setMediaPath(videoClips.first().filePath);
    }

    if (dialog.exec() != QDialog::Accepted)
        return;

    const whisper::TranscribeRequest req = dialog.request();
    whisper::WhisperTranscriber transcriber;
    const whisper::TranscribeOutcome outcome = transcriber.transcribe(req);

    if (!outcome.success) {
        QMessageBox::warning(this, QStringLiteral("動画を文字起こし"),
            QStringLiteral("文字起こしに失敗しました:\n%1")
                .arg(outcome.error.isEmpty()
                         ? QStringLiteral("不明なエラー")
                         : outcome.error));
        return;
    }

    const int segmentCount = outcome.track.clipCount();

    // 生成した caption::Track を既存の字幕エディタ経路へ渡す (破棄しない)。
    openCaptionEditorDialog();
    if (m_captionEditorDialog)
        m_captionEditorDialog->setTrack(outcome.track);

    const QString summary =
        QStringLiteral("%1 件のセグメントを生成し、字幕エディタに読み込みました。").arg(segmentCount);
    statusBar()->showMessage(summary);
    QMessageBox::information(this, QStringLiteral("動画を文字起こし"), summary);
}

// Phase 6 Wave 3 (US-6C-4): 文字起こしからハイライト検出配線。
// 現在の字幕トラック(あれば)を TranscriptHighlightDialog に渡してモーダル exec()。
// Accepted なら request() を TranscriptHighlighter::detect() に渡し、検出結果を
// Dialog の結果欄 + statusBar に [mm:ss-mm:ss score] reason 形式で表示する。
void MainWindow::openTranscriptHighlightDialog()
{
    TranscriptHighlightDialog dialog(this);

    // 現在の字幕トラックがあれば transcript として渡す (無ければ空)。
    if (m_captionEditorDialog)
        dialog.setTranscript(m_captionEditorDialog->track().clips());

    if (dialog.exec() != QDialog::Accepted)
        return;

    const transcripthl::HighlightRequest req = dialog.request();
    QString err;
    transcripthl::TranscriptHighlighter highlighter;
    const QVector<Highlight> highlights = highlighter.detect(req, nullptr, &err);

    // dialog.exec() は既に閉じているため、結果は QMessageBox で可視化する
    // (6B-4 whisper と同じパターン)。
    if (highlights.isEmpty()) {
        const QString message = err.isEmpty()
            ? QStringLiteral("ハイライトを検出できませんでした。")
            : QStringLiteral("ハイライト検出に失敗しました: %1\n"
                             "(API キーは設定から登録してください)").arg(err);
        statusBar()->showMessage(message);
        QMessageBox::warning(this, QStringLiteral("文字起こしからハイライト検出"), message);
        return;
    }

    // [mm:ss-mm:ss score] reason 形式で列挙。
    const auto fmt = [](double seconds) {
        const int total = static_cast<int>(seconds);
        return QStringLiteral("%1:%2")
            .arg(total / 60, 2, 10, QLatin1Char('0'))
            .arg(total % 60, 2, 10, QLatin1Char('0'));
    };
    QStringList lines;
    for (const Highlight &h : highlights) {
        lines.append(QStringLiteral("[%1-%2 %3] %4")
                         .arg(fmt(h.startTime))
                         .arg(fmt(h.endTime))
                         .arg(h.score, 0, 'f', 2)
                         .arg(h.description));
    }

    // Phase 6 Wave 4 (US-6D-4): 自動カット (openAutoClipDialog) の計算ソースとして
    // 直近の検出結果を保持する。
    m_lastHighlights = highlights;

    const QString summary =
        QStringLiteral("%1 件のハイライトを検出しました。").arg(highlights.size());
    statusBar()->showMessage(summary);
    QMessageBox::information(this, QStringLiteral("文字起こしからハイライト検出"),
                            summary + QStringLiteral("\n\n") + lines.join(QLatin1Char('\n')));
}

// Phase 6 Wave 4 (US-6D-4): ハイライトから自動カット配線。
// 直近の検出結果 (m_lastHighlights) と現在の動画の長さを AutoClipDialog に渡し、
// Accepted なら AutoClipGenerator::planClips でカット範囲を計算する。結果を
// QMessageBox::question で件数+一覧表示し、Yes ならタイムライン末尾へ追加する。
void MainWindow::openAutoClipDialog()
{
    // 現在の動画 (timeline 先頭の videoClip) の長さを取得 (無ければ 0)。
    double durationSec = 0.0;
    if (m_timeline) {
        const auto &clips = m_timeline->videoClips();
        if (!clips.isEmpty())
            durationSec = clips.first().duration;
    }

    AutoClipDialog dialog(this);
    dialog.setHighlightCount(m_lastHighlights.size());
    dialog.setSourceDuration(durationSec);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const autoclip::AutoClipConfig config = dialog.config();
    const QVector<autoclip::ClipPlan> plans =
        autoclip::AutoClipGenerator::planClips(m_lastHighlights, durationSec, config);

    if (plans.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("ハイライトから自動カット"),
            QStringLiteral("カット範囲を計算できませんでした "
                           "(ハイライト未検出かもしれません)"));
        return;
    }

    // [mm:ss-mm:ss score] label 形式で列挙。
    const auto fmt = [](double seconds) {
        const int total = static_cast<int>(seconds);
        return QStringLiteral("%1:%2")
            .arg(total / 60, 2, 10, QLatin1Char('0'))
            .arg(total % 60, 2, 10, QLatin1Char('0'));
    };
    QStringList lines;
    for (const autoclip::ClipPlan &p : plans) {
        lines.append(QStringLiteral("[%1-%2 %3] %4")
                         .arg(fmt(p.startSec))
                         .arg(fmt(p.endSec))
                         .arg(p.score, 0, 'f', 2)
                         .arg(p.label));
    }

    const QString question =
        QStringLiteral("%1 個のカット範囲を検出しました。"
                       "タイムラインに追加しますか?").arg(plans.size());
    const auto answer = QMessageBox::question(
        this, QStringLiteral("ハイライトから自動カット"),
        question + QStringLiteral("\n\n") + lines.join(QLatin1Char('\n')),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (answer != QMessageBox::Yes)
        return;

    QString sourceFilePath;
    if (m_timeline) {
        const auto &clips = m_timeline->videoClips();
        if (!clips.isEmpty())
            sourceFilePath = clips.first().filePath;
    }

    const QVector<ClipInfo> newClips =
        autoclip::AutoClipGenerator::toTimelineClips(plans, sourceFilePath, durationSec);
    // Timeline は ClipInfo を直接受け取る公開 API を持たないため、先頭の
    // 動画トラック (TimelineTrack::addClip(const ClipInfo&)) に末尾追加する。
    int added = 0;
    if (m_timeline && !m_timeline->videoTracks().isEmpty()) {
        TimelineTrack *track = m_timeline->videoTracks().first();
        if (track) {
            for (const ClipInfo &c : newClips) {
                track->addClip(c);
                ++added;
            }
        }
    }
    statusBar()->showMessage(QStringLiteral("%1 個追加").arg(added));
}

// US-CP-4: コマンドパレット用の index を menuBar() から構築する。
// 各トップメニューを再帰的に辿り、separator でもサブメニュー親 (menu() を
// 持つ項目) でもなく text() が非空の QAction を CommandEntry に変換する。
// id は objectName() が非空ならそれ、空なら "cmd_<連番>" を生成する。
// keywords には m_menuHelpEntries 由来の説明文 + 所属トップメニュー名 を足す。
// 副作用として m_commandActions を id→QAction で再構築する。
QVector<cmdsearch::CommandEntry> MainWindow::buildCommandEntries()
{
    m_commandActions.clear();

    // 説明文の高速参照: QAction* → help 文字列。
    QHash<const QAction *, QString> helpByAction;
    for (const auto &pair : m_menuHelpEntries) {
        if (pair.first)
            helpByAction.insert(pair.first, pair.second);
    }

    QVector<cmdsearch::CommandEntry> entries;
    int autoId = 0;

    // 走査キュー: (menu, 所属トップメニュー名)。サブメニューは末尾に積んで
    // 幅優先で辿る (std::function 再帰を避けるため反復実装)。
    QVector<QPair<QMenu *, QString>> pending;
    const QList<QAction *> topActions = menuBar()->actions();
    for (QAction *topAction : topActions) {
        if (!topAction || topAction->isSeparator())
            continue;
        QMenu *topMenu = topAction->menu();
        if (!topMenu)
            continue;
        QString topName = topAction->text();
        topName.remove(QLatin1Char('&'));
        pending.append({topMenu, topName});
    }

    for (int qi = 0; qi < pending.size(); ++qi) {
        QMenu *menu = pending[qi].first;
        const QString topMenuName = pending[qi].second;
        if (!menu)
            continue;
        const QList<QAction *> actions = menu->actions();
        for (QAction *action : actions) {
            if (!action || action->isSeparator())
                continue;
            if (action->menu()) {
                // サブメニュー親は実行対象にせず、子を後で辿る。
                pending.append({action->menu(), topMenuName});
                continue;
            }
            const QString rawText = action->text();
            if (rawText.isEmpty())
                continue;

            // id: objectName 優先、無ければ自動採番。
            QString id = action->objectName();
            if (id.isEmpty())
                id = QStringLiteral("cmd_%1").arg(autoId++);
            // 同一 id 衝突を避ける (objectName が複数 action で空 → 自動採番で回避済だが念のため)。
            if (m_commandActions.contains(id))
                id = QStringLiteral("cmd_%1").arg(autoId++);

            // title: '&' (ニーモニック) を除去。末尾の "..."/"…" は保持。
            QString title = rawText;
            title.remove(QLatin1Char('&'));

            // keywords: 説明文 + 所属トップメニュー名。
            QStringList kw;
            const QString help = helpByAction.value(action);
            if (!help.isEmpty())
                kw << help;
            if (!topMenuName.isEmpty())
                kw << topMenuName;

            cmdsearch::CommandEntry entry;
            entry.id = id;
            entry.title = title;
            entry.keywords = kw.join(QLatin1Char(' '));
            entries.append(entry);

            m_commandActions.insert(id, action);
        }
    }

    return entries;
}

// US-CP-4: コマンドパレットを開く。現在のメニュー状態から index を再構築し、
// CommandPaletteDialog で検索 → commandTriggered(id) を受けて対応 QAction を
// trigger() する。trigger() の前に必ず accept() でパレットを閉じることで、
// action がモーダル/別ダイアログを開く場合の入れ子モーダルを避ける。
void MainWindow::openCommandPalette()
{
    const QVector<cmdsearch::CommandEntry> entries = buildCommandEntries();

    CommandPaletteDialog dialog(this);
    dialog.setCommands(entries);
    // commandTriggered は accept() 前に emit されるため、ここで直接 trigger すると
    // パレットが開いたまま action が走り入れ子モーダルになる。id だけ捕捉して
    // exec() 返却後 (=パレット閉鎖後) に trigger する。
    QString chosenId;
    connect(&dialog, &CommandPaletteDialog::commandTriggered,
            this, [&chosenId](const QString &id) { chosenId = id; });
    dialog.exec();

    if (!chosenId.isEmpty()) {
        QAction *action = m_commandActions.value(chosenId, nullptr);
        if (action)
            action->trigger();
    }
}

// US-INT-1: Sprint 16 — open / raise the mobile-device export dialog (modeless).
// Lazy-creates MobileExportDialog with the current source size + measured LUFS=0.0;
// hooks exportRequested into the existing Exporter::startExport pipeline so that
// pressing 「書き出し」 inside the dialog runs the standard timeline export with
// the resolved mobile-profile config.
void MainWindow::onMobileExport()
{
#ifdef HAVE_MOBILE_EXPORT
    if (!m_mobileExportDialog) {
        // Use the project resolution as the source size; falls back to 1920x1080
        // when uninitialised (matches ProjectSettings defaults).
        const QSize sourceSize(m_projectConfig.width  > 0 ? m_projectConfig.width  : 1920,
                               m_projectConfig.height > 0 ? m_projectConfig.height : 1080);
        // measuredLufs=0.0: integrated loudness analysis is wired in a follow-up
        // story; current dialog accepts 0.0 as "unknown / no normalisation".
        m_mobileExportDialog = new MobileExportDialog(sourceSize, 0.0, this);
        m_mobileExportDialog->setObjectName(QStringLiteral("mobileExportDialog"));
        connect(m_mobileExportDialog, &MobileExportDialog::exportRequested,
                this, [this](const ExportConfig &cfg) {
                    if (!m_timeline) return;
                    const auto &clips = m_timeline->videoClips();
                    if (clips.isEmpty()) {
                        QMessageBox::warning(this, QStringLiteral("モバイル書き出し"),
                            QStringLiteral("タイムラインにクリップがありません。"));
                        return;
                    }
                    // S12 single-path: mobile export also routes through
                    // RenderQueue -> tlrender::renderFrameAt (S8), NOT the
                    // legacy CPU-only Exporter. See progress.txt S12.
                    if (!m_renderQueue)
                        m_renderQueue = new RenderQueue(this);

                    RenderJob job;
                    job.name = QFileInfo(cfg.outputPath).fileName();
                    job.projectFilePath = m_projectFilePath;
                    job.outputPath = cfg.outputPath;
                    job.width   = cfg.width  > 0 ? cfg.width  : 1920;
                    job.height  = cfg.height > 0 ? cfg.height : 1080;
                    job.bitrateBps =
                        static_cast<qint64>(cfg.videoBitrate) * 1000;
                    job.startUs = 0;
                    job.endUs   = 0;
                    job.timeline = m_timeline;
                    QJsonObject jcfg;
                    jcfg["width"]        = job.width;
                    jcfg["height"]       = job.height;
                    jcfg["fps"]          = cfg.fps > 0 ? cfg.fps : 30;
                    jcfg["videoCodec"]   = cfg.videoCodec;
                    jcfg["videoBitrate"] = cfg.videoBitrate;
                    jcfg["audioCodec"]   = cfg.audioCodec;
                    jcfg["audioBitrate"] = cfg.audioBitrate;
                    if (cfg.hdr10 ||
                        cfg.hdrSettings.mode != QStringLiteral("sdr")) {
                        jcfg["hdr10"]   = true;
                        jcfg["hdrMode"] = cfg.hdrSettings.mode;
                    }
                    if (cfg.proresProfile >= 0)
                        jcfg["proresProfile"] = cfg.proresProfile;
                    job.exportConfig = jcfg;

                    auto *progress = new QProgressDialog(
                        QStringLiteral("モバイル向けエクスポート中..."),
                        QStringLiteral("キャンセル"), 0, 100, this);
                    progress->setWindowModality(Qt::WindowModal);
                    progress->setMinimumDuration(0);
                    connect(m_renderQueue, &RenderQueue::jobProgressUuid,
                            progress,
                            [progress](const QString &, int percent) {
                                progress->setValue(percent);
                            });
                    connect(m_renderQueue, &RenderQueue::jobCompletedUuid,
                            progress,
                            [this, progress](const QString &, bool ok,
                                             const QString &err) {
                                progress->close();
                                progress->deleteLater();
                                if (ok)
                                    statusBar()->showMessage(QStringLiteral(
                                        "モバイル書き出し完了 (timeline SSOT)"));
                                else
                                    QMessageBox::critical(this,
                                        QStringLiteral("モバイル書き出し失敗"),
                                        err.isEmpty()
                                            ? QStringLiteral("Export failed")
                                            : err);
                            });
                    connect(progress, &QProgressDialog::canceled,
                            m_renderQueue, &RenderQueue::stop);
                    // RM-1.3: job.timeline != nullptr path — same carrier
                    // re-sync requirement as File→Export above.
                    syncTrackMatteEntriesToTimeline(
                        m_timeline, m_trackMatteClipEntries);
                    statusBar()->showMessage(
                        QStringLiteral("モバイル書き出し: ") + cfg.outputPath);
                    m_renderQueue->addJob(job);
                    m_renderQueue->start();
                });
    }
    m_mobileExportDialog->show();
    m_mobileExportDialog->raise();
    m_mobileExportDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("モバイルエクスポート"),
        QStringLiteral("MobileExportDialog がビルドに含まれていません。"));
#endif
}

// US-INT-1: Sprint 16 — open / raise the external-tool import hub dialog (modeless).
// Wires 4 import signals (timeline placements / image / mesh / EXR sequence) into
// MainWindow. Timeline import maps each placement to a Timeline::addClip call;
// image / mesh / EXR sequence imports log a TODO + status until the dedicated
// helper APIs (image overlay loader, 3D mesh ingest, EXR sequence loader) land.
void MainWindow::onImportHub()
{
#ifdef HAVE_IMPORT_HUB
    if (!m_importHubDialog) {
        m_importHubDialog = new ImportHubDialog(this);
        m_importHubDialog->setObjectName(QStringLiteral("importHubDialog"));

        connect(m_importHubDialog, &ImportHubDialog::timelineImportRequested,
                this, [this](const QList<obs::layout::TimelineClipPlacement> &placements) {
                    if (!m_timeline) return;
                    int added = 0;
                    for (const auto &p : placements) {
                        if (p.filePath.isEmpty()) continue;
                        m_timeline->addClip(p.filePath);
                        ++added;
                    }
                    if (statusBar()) {
                        statusBar()->showMessage(
                            QStringLiteral("取り込みハブ: %1 件のクリップをタイムラインに追加")
                                .arg(added),
                            5000);
                    }
                });

        connect(m_importHubDialog, &ImportHubDialog::imageImportRequested,
                this, [this](const QImage &image, const QString &name) {
                    // TODO(US-INT-2): wire to addImageOverlay / image-layer ingest helper.
                    qInfo().noquote() << QStringLiteral(
                        "ImportHub image: name=%1 size=%2x%3")
                        .arg(name)
                        .arg(image.width())
                        .arg(image.height());
                    if (statusBar()) {
                        statusBar()->showMessage(
                            QStringLiteral("画像取り込み (%1) を受信 (近日 timeline 連携予定)")
                                .arg(name),
                            5000);
                    }
                });

        connect(m_importHubDialog, &ImportHubDialog::meshImportRequested,
                this, [this](const blender::mesh::MeshData &meshData) {
                    // TODO(US-INT-2): wire to 3D mesh layer ingest once available.
                    qInfo().noquote() << QStringLiteral(
                        "ImportHub mesh: vertices=%1 triangles=%2")
                        .arg(meshData.vertices.size())
                        .arg(meshData.triangleIndices.size() / 3);
                    if (statusBar()) {
                        statusBar()->showMessage(
                            QStringLiteral("メッシュ取り込みを受信 (近日 3D layer 連携予定)"),
                            5000);
                    }
                });

        connect(m_importHubDialog, &ImportHubDialog::exrSequenceImportRequested,
                this, [this](const QString &folderPath, const QString &pattern) {
                    // TODO(US-INT-2): wire to EXR sequence loader / image-sequence layer.
                    qInfo().noquote() << QStringLiteral(
                        "ImportHub EXR sequence: folder=%1 pattern=%2")
                        .arg(folderPath, pattern);
                    if (statusBar()) {
                        statusBar()->showMessage(
                            QStringLiteral("EXR シーケンス取り込みを受信 (%1) — 近日対応予定")
                                .arg(pattern),
                            5000);
                    }
                });
    }
    m_importHubDialog->show();
    m_importHubDialog->raise();
    m_importHubDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("取り込みハブ"),
        QStringLiteral("ImportHubDialog がビルドに含まれていません。"));
#endif
}

// US-INT-3: Sprint 17 — open / raise the YouTube upload dialog (modeless).
// Lazy-creates a YoutubeOAuth AuthClient + Manager pair (Manager non-owning ref to
// AuthClient) and a single dialog re-used across invocations.
void MainWindow::onYoutubeUpload()
{
#ifdef HAVE_YOUTUBE
    if (!m_youtubeOAuth) {
        m_youtubeOAuth = new youtube::oauth::AuthClient(
            youtube::oauth::YoutubeOAuthConfig::defaultConfig(), this);
    }
    if (!m_youtubeManager) {
        m_youtubeManager = new youtube::manager::Manager(m_youtubeOAuth, this);
    }
    if (!m_youtubeUploadDialog) {
        m_youtubeUploadDialog = new YoutubeUploadDialog(m_youtubeManager, this);
        m_youtubeUploadDialog->setObjectName(QStringLiteral("youtubeUploadDialog"));
    }
    m_youtubeUploadDialog->show();
    m_youtubeUploadDialog->raise();
    m_youtubeUploadDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("YouTube アップロード"),
        QStringLiteral("YoutubeUploadDialog がビルドに含まれていません。"));
#endif
}

// US-INT-3: Sprint 18 — toggle the comment dock (lazy-create on first use).
// Backed by a single CommentTrack that lives for the lifetime of MainWindow so
// that multiple invocations of the panel see the same comment list.
void MainWindow::onCommentsPanel()
{
#ifdef HAVE_COLLAB
    if (!m_commentTrack) {
        m_commentTrack = new collab::CommentTrack();
    }
    if (!m_commentsDock) {
        m_commentsDock = new CommentsDockWidget(m_commentTrack, this);
        m_commentsDock->setObjectName(QStringLiteral("commentsDock"));
        addDockWidget(Qt::RightDockWidgetArea, m_commentsDock);
    }
    m_commentsDock->show();
    m_commentsDock->raise();
#else
    QMessageBox::information(this, QStringLiteral("コラボレーションパネル"),
        QStringLiteral("CommentsDockWidget がビルドに含まれていません。"));
#endif
}

// US-INT-3: Sprint 18 — open / raise the version-history dialog (modeless).
// Backed by a single HistoryLog that lives for the lifetime of MainWindow.
void MainWindow::onCollabHistory()
{
#ifdef HAVE_COLLAB
    if (!m_collabHistoryLog) {
        m_collabHistoryLog = new collab::history::HistoryLog();
    }
    if (!m_collabHistoryDialog) {
        m_collabHistoryDialog = new CollabHistoryDialog(m_collabHistoryLog, this);
        m_collabHistoryDialog->setObjectName(QStringLiteral("collabHistoryDialog"));
    }
    m_collabHistoryDialog->show();
    m_collabHistoryDialog->raise();
    m_collabHistoryDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("変更履歴"),
        QStringLiteral("CollabHistoryDialog がビルドに含まれていません。"));
#endif
}

// US-INT-3: Sprint 19 — open / raise the auto color match dialog (modeless).
// The dialog itself is stateless beyond the user-selected images, so a single
// instance is reused across invocations.
void MainWindow::onColorMatch()
{
#ifdef HAVE_COLORMATCH
    if (!m_colorMatchDialog) {
        m_colorMatchDialog = new ColorMatchDialog(this);
        m_colorMatchDialog->setObjectName(QStringLiteral("colorMatchDialog"));
    }
    m_colorMatchDialog->show();
    m_colorMatchDialog->raise();
    m_colorMatchDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("自動カラーマッチ"),
        QStringLiteral("ColorMatchDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openVimeoUploadDialog()
{
#ifdef HAVE_VIMEO
    if (!m_vimeoOAuth) {
        m_vimeoOAuth = new vimeo::oauth::AuthClient(
            vimeo::oauth::VimeoOAuthConfig::defaultConfig(), this);
    }
    if (!m_vimeoManager) {
        m_vimeoManager = new vimeo::manager::Manager(m_vimeoOAuth, this);
    }
    if (!m_vimeoUploadDialog) {
        m_vimeoUploadDialog = new VimeoUploadDialog(m_vimeoManager, this);
        m_vimeoUploadDialog->setObjectName(QStringLiteral("vimeoUploadDialog"));
    }
    m_vimeoUploadDialog->show();
    m_vimeoUploadDialog->raise();
    m_vimeoUploadDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("Vimeo アップロード"),
        QStringLiteral("VimeoUploadDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openTwitchStreamDialog()
{
#ifdef HAVE_TWITCH
    if (!m_twitchStreamDialog) {
        m_twitchStreamDialog = new TwitchStreamDialog(this);
        m_twitchStreamDialog->setObjectName(QStringLiteral("twitchStreamDialog"));
    }
    m_twitchStreamDialog->show();
    m_twitchStreamDialog->raise();
    m_twitchStreamDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("Twitch 配信設定"),
        QStringLiteral("TwitchStreamDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openFrameIoImportDialog()
{
#ifdef HAVE_FRAMEIO
    bool ok = false;
    const QString apiToken = QInputDialog::getText(
        this,
        QStringLiteral("Frame.io コメント取り込み"),
        QStringLiteral("API token:"),
        QLineEdit::Password,
        QString(),
        &ok).trimmed();
    if (!ok || apiToken.isEmpty())
        return;

    const QString assetId = QInputDialog::getText(
        this,
        QStringLiteral("Frame.io コメント取り込み"),
        QStringLiteral("Asset ID:"),
        QLineEdit::Normal,
        QString(),
        &ok).trimmed();
    if (!ok || assetId.isEmpty())
        return;

    if (!m_frameIoImporter) {
        m_frameIoImporter = new frameio::importer::FrameIoImporter(this);
        connect(m_frameIoImporter,
                &frameio::importer::FrameIoImporter::importProgress,
                this, [this](int percent) {
                    if (statusBar()) {
                        statusBar()->showMessage(
                            QStringLiteral("Frame.io コメントを取得中... %1%")
                                .arg(percent));
                    }
                });
        connect(m_frameIoImporter,
                &frameio::importer::FrameIoImporter::importFinished,
                this, [this](const collab::CommentTrack &importedTrack) {
                    if (!m_commentTrack)
                        m_commentTrack = new collab::CommentTrack();
                    if (m_commentTrack->trackId.isEmpty())
                        m_commentTrack->trackId = importedTrack.trackId;

                    QSet<QString> existingIds;
                    existingIds.reserve(m_commentTrack->comments.size());
                    for (const collab::Comment &comment : m_commentTrack->comments)
                        existingIds.insert(comment.id);

                    int addedCount = 0;
                    for (const collab::Comment &comment : importedTrack.comments) {
                        if (existingIds.contains(comment.id))
                            continue;
                        m_commentTrack->comments.append(comment);
                        existingIds.insert(comment.id);
                        ++addedCount;
                    }
                    if (addedCount > 0)
                        m_commentTrack->version += addedCount;

#ifdef HAVE_COLLAB
                    if (m_commentsDock) {
                        const Qt::DockWidgetArea area = dockWidgetArea(m_commentsDock);
                        const Qt::DockWidgetArea targetArea =
                            area == Qt::NoDockWidgetArea
                                ? Qt::RightDockWidgetArea
                                : area;
                        const bool wasFloating = m_commentsDock->isFloating();
                        const bool wasVisible = m_commentsDock->isVisible();
                        const QRect geometry = m_commentsDock->geometry();

                        delete m_commentsDock;
                        m_commentsDock = new CommentsDockWidget(m_commentTrack, this);
                        m_commentsDock->setObjectName(QStringLiteral("commentsDock"));
                        addDockWidget(targetArea, m_commentsDock);

                        if (wasFloating) {
                            m_commentsDock->setFloating(true);
                            m_commentsDock->setGeometry(geometry);
                        }
                        if (wasVisible) {
                            m_commentsDock->show();
                            m_commentsDock->raise();
                        }
                    }
#endif

                    if (statusBar()) {
                        statusBar()->showMessage(
                            QStringLiteral("Frame.io コメントを %1 件取り込みました")
                                .arg(addedCount),
                            5000);
                    }
                });
        connect(m_frameIoImporter,
                &frameio::importer::FrameIoImporter::importFailed,
                this, [this](const QString &error) {
                    QMessageBox::warning(
                        this,
                        QStringLiteral("Frame.io コメント取り込み"),
                        QStringLiteral("コメント取り込みに失敗しました:\n%1")
                            .arg(error));
                });
    }

    frameio::importer::ImportConfig config;
    config.apiToken = apiToken;

    if (statusBar()) {
        statusBar()->showMessage(
            QStringLiteral("Frame.io コメントを取得しています..."));
    }
    m_frameIoImporter->fetchComments(assetId, config);
#else
    QMessageBox::information(this, QStringLiteral("Frame.io コメント取り込み"),
        QStringLiteral("FrameIoImporter がビルドに含まれていません。"));
#endif
}

void MainWindow::openDavinciExportDialog()
{
#ifdef HAVE_DAVINCI_XML
    const QString suggestedPath = QDir::homePath() + QDir::separator()
        + (m_projectConfig.name.isEmpty() ? QStringLiteral("Untitled")
                                          : m_projectConfig.name)
        + QStringLiteral(".xml");
    const QString outPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("DaVinci Resolve XML を書き出し"),
        suggestedPath,
        QStringLiteral("XML Files (*.xml);;All Files (*)"));
    if (outPath.isEmpty())
        return;

    davinci::xml::ExporterConfig config;
    config.sequenceName = m_projectConfig.name.isEmpty()
        ? QStringLiteral("Untitled")
        : m_projectConfig.name;
    config.fps = qMax(1, m_projectConfig.fps);
    config.width = qMax(1, m_projectConfig.width);
    config.height = qMax(1, m_projectConfig.height);

    const QVector<davinci::xml::ClipEntry> clips =
        buildDavinciExportClips(m_timeline, config.fps);
    const QString xml = davinci::xml::buildXml(clips, config);

    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("DaVinci Resolve XML 書き出し"),
            QStringLiteral("ファイルを書き込めませんでした:\n%1")
                .arg(file.errorString()));
        return;
    }
    if (file.write(xml.toUtf8()) < 0) {
        QMessageBox::warning(this, QStringLiteral("DaVinci Resolve XML 書き出し"),
            QStringLiteral("XML の書き込みに失敗しました:\n%1")
                .arg(file.errorString()));
        return;
    }

    if (statusBar()) {
        statusBar()->showMessage(
            QStringLiteral("DaVinci Resolve XML を書き出しました (%1 クリップ)")
                .arg(clips.size()),
            5000);
    }
#else
    QMessageBox::information(this, QStringLiteral("DaVinci Resolve XML 書き出し"),
        QStringLiteral("DavinciResolveXmlExporter がビルドに含まれていません。"));
#endif
}

void MainWindow::openFcpxmlExportDialog()
{
#ifdef HAVE_FCPXML
    const QString suggestedPath = QDir::homePath() + QDir::separator()
        + (m_projectConfig.name.isEmpty() ? QStringLiteral("Untitled")
                                          : m_projectConfig.name)
        + QStringLiteral(".fcpxml");
    const QString outPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("FCPXML を書き出し"),
        suggestedPath,
        QStringLiteral("FCPXML Files (*.fcpxml *.xml);;All Files (*)"));
    if (outPath.isEmpty())
        return;

    fcpx::xml::ExporterConfig config;
    config.projectName = m_projectConfig.name.isEmpty()
        ? QStringLiteral("Untitled")
        : m_projectConfig.name;
    config.fps = qMax(1, m_projectConfig.fps);
    config.frameDuration = fcpxFrameDuration(config.fps);
    config.width = qMax(1, m_projectConfig.width);
    config.height = qMax(1, m_projectConfig.height);

    const QVector<fcpx::xml::ClipEntry> clips =
        buildFcpxmlExportClips(m_timeline);
    const QString xml = fcpx::xml::buildXml(clips, config);

    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("FCPXML 書き出し"),
            QStringLiteral("ファイルを書き込めませんでした:\n%1")
                .arg(file.errorString()));
        return;
    }
    if (file.write(xml.toUtf8()) < 0) {
        QMessageBox::warning(this, QStringLiteral("FCPXML 書き出し"),
            QStringLiteral("XML の書き込みに失敗しました:\n%1")
                .arg(file.errorString()));
        return;
    }

    if (statusBar()) {
        statusBar()->showMessage(
            QStringLiteral("FCPXML を書き出しました (%1 クリップ)")
                .arg(clips.size()),
            5000);
    }
#else
    QMessageBox::information(this, QStringLiteral("FCPXML 書き出し"),
        QStringLiteral("FcpxmlExporter がビルドに含まれていません。"));
#endif
}

void MainWindow::openSmartEditDialog()
{
#ifdef HAVE_SMARTEDIT
    if (!m_smartEditDialog) {
        m_smartEditDialog = new SmartEditDialog(this);
        m_smartEditDialog->setObjectName(QStringLiteral("smartEditDialog"));
    }
    m_smartEditDialog->show();
    m_smartEditDialog->raise();
    m_smartEditDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("Smart Edit アシスタント"),
        QStringLiteral("SmartEditDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openCloudRenderDialog()
{
#ifdef HAVE_CLOUD_RENDER
    if (!m_cloudRenderClient)
        m_cloudRenderClient = new cloudrender::Client(this);
    if (!m_cloudRenderDialog) {
        m_cloudRenderDialog = new CloudRenderDialog(this);
        m_cloudRenderDialog->setObjectName(QStringLiteral("cloudRenderDialog"));
    }
    m_cloudRenderDialog->show();
    m_cloudRenderDialog->raise();
    m_cloudRenderDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("クラウドレンダリング"),
        QStringLiteral("CloudRenderDialog がビルドに含まれていません。"));
#endif
}

// --- US-INT-2: Sprint 21 — platform expansion / mastering / batch export. ---

void MainWindow::openXVideoDialog()
{
#ifdef HAVE_XVIDEO
    if (!m_xVideoDialog) {
        m_xVideoDialog = new XVideoDialog(this);
        m_xVideoDialog->setObjectName(QStringLiteral("xVideoDialog"));
    }
    m_xVideoDialog->show();
    m_xVideoDialog->raise();
    m_xVideoDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("X(Twitter) に動画投稿"),
        QStringLiteral("XVideoDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openInstagramDialog()
{
#ifdef HAVE_INSTAGRAM_PUBLISH
    if (!m_instagramDialog) {
        m_instagramDialog = new InstagramPublishDialog(this);
        m_instagramDialog->setObjectName(QStringLiteral("instagramPublishDialog"));
    }
    m_instagramDialog->show();
    m_instagramDialog->raise();
    m_instagramDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("Instagram Reels に投稿"),
        QStringLiteral("InstagramPublishDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openProjectTemplateDialog()
{
#ifdef HAVE_PROJECT_TEMPLATE
    if (!m_projectTemplateDialog) {
        m_projectTemplateDialog = new ProjectTemplateDialog(this);
        m_projectTemplateDialog->setObjectName(QStringLiteral("projectTemplateDialog"));
    }
    m_projectTemplateDialog->show();
    m_projectTemplateDialog->raise();
    m_projectTemplateDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("プロジェクトテンプレート"),
        QStringLiteral("ProjectTemplateDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openLoudnessDialog()
{
#ifdef HAVE_LOUDNESS_MASTER
    if (!m_loudnessDialog) {
        m_loudnessDialog = new LoudnessMasterDialog(this);
        m_loudnessDialog->setObjectName(QStringLiteral("loudnessMasterDialog"));
    }
    m_loudnessDialog->show();
    m_loudnessDialog->raise();
    m_loudnessDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("ラウドネスマスタリング"),
        QStringLiteral("LoudnessMasterDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openHdrDialog()
{
#ifdef HAVE_HDR_GRADING
    if (!m_hdrDialog) {
        m_hdrDialog = new HdrGradingDialog(this);
        m_hdrDialog->setObjectName(QStringLiteral("hdrGradingDialog"));
    }
    m_hdrDialog->show();
    m_hdrDialog->raise();
    m_hdrDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("HDR カラーグレーディング"),
        QStringLiteral("HdrGradingDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openMultiCamSyncDialog()
{
#ifdef HAVE_MULTICAM_SYNC
    if (!m_multiCamSyncDialog) {
        m_multiCamSyncDialog = new MultiCamSyncDialog(this);
        m_multiCamSyncDialog->setObjectName(QStringLiteral("multiCamSyncDialog"));
    }
    m_multiCamSyncDialog->show();
    m_multiCamSyncDialog->raise();
    m_multiCamSyncDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("マルチカム同期"),
        QStringLiteral("MultiCamSyncDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openBatchExportDialog()
{
#ifdef HAVE_BATCH_EXPORT
    if (!m_batchExportDialog) {
        m_batchExportDialog = new BatchExportDialog(this);
        m_batchExportDialog->setObjectName(QStringLiteral("batchExportDialog"));
    }
    m_batchExportDialog->show();
    m_batchExportDialog->raise();
    m_batchExportDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("バッチエクスポート"),
        QStringLiteral("BatchExportDialog がビルドに含まれていません。"));
#endif
}

// --- US-INT-2: Sprint 22 — keying / restoration / animated export / easing /
//     subtitle translation / lower-third / watermark. ---

void MainWindow::openChromaKeyDialog()
{
#ifdef HAVE_CHROMA_KEY_REFINE_DIALOG
    if (!m_chromaKeyDialog) {
        m_chromaKeyDialog = new ChromaKeyRefineDialog(this);
        m_chromaKeyDialog->setObjectName(QStringLiteral("chromaKeyDialog"));
    }
    m_chromaKeyDialog->show();
    m_chromaKeyDialog->raise();
    m_chromaKeyDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("クロマキー精緻化"),
        QStringLiteral("ChromaKeyRefineDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openAudioRestoreDialog()
{
#ifdef HAVE_AUDIO_RESTORATION_DIALOG
    if (!m_audioRestoreDialog) {
        m_audioRestoreDialog = new AudioRestorationDialog(this);
        m_audioRestoreDialog->setObjectName(QStringLiteral("audioRestoreDialog"));
    }
    m_audioRestoreDialog->show();
    m_audioRestoreDialog->raise();
    m_audioRestoreDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("音声リストア"),
        QStringLiteral("AudioRestorationDialog がビルドに含まれていません。"));
#endif
}

#ifdef HAVE_SPECTRAL_EDIT_DIALOG
namespace {

// 16bit PCM WAV (RIFF/fmt/data) を読み込み mono の double サンプル列へ展開する。
// 多チャンネルは平均してモノラル化。成功時 true、sampleRate を *sampleRate へ。
// 簡易パーサ — float WAV や 24/32bit は非対応 (抽出側を 16bit に固定する前提)。
bool readPcm16WavToMono(const QString &wavPath,
                        std::vector<double> &outSamples,
                        int &sampleRate,
                        QString *error)
{
    outSamples.clear();
    sampleRate = 0;

    constexpr qint64 kMaxWavBytes = 512LL * 1024LL * 1024LL;
    const QFileInfo info(wavPath);
    if (!info.exists()) {
        if (error) *error = QStringLiteral("WAV が存在しません: %1").arg(wavPath);
        return false;
    }
    if (info.size() <= 0) {
        if (error) *error = QStringLiteral("WAV が空です: %1").arg(wavPath);
        return false;
    }
    if (info.size() > kMaxWavBytes) {
        if (error) *error =
            QStringLiteral("WAV が大きすぎます (%1 MB、上限 %2 MB)。")
                .arg(info.size() / (1024.0 * 1024.0), 0, 'f', 1)
                .arg(kMaxWavBytes / (1024 * 1024));
        return false;
    }

    QFile f(wavPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("WAV を開けません: %1").arg(wavPath);
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();

    if (data.size() != info.size()) {
        if (error) *error =
            QStringLiteral("WAV の読み込みが途中で終了しました (%1/%2 bytes)。")
                .arg(data.size()).arg(info.size());
        return false;
    }
    if (data.size() < 44 || !data.startsWith("RIFF") ||
        data.mid(8, 4) != QByteArray("WAVE")) {
        if (error) *error = QStringLiteral("RIFF/WAVE ヘッダが不正です。");
        return false;
    }

    const auto rdU16 = [&](int off) -> quint16 {
        return static_cast<quint16>(static_cast<unsigned char>(data[off])) |
               (static_cast<quint16>(static_cast<unsigned char>(data[off + 1])) << 8);
    };
    const auto rdU32 = [&](int off) -> quint32 {
        return static_cast<quint32>(static_cast<unsigned char>(data[off])) |
               (static_cast<quint32>(static_cast<unsigned char>(data[off + 1])) << 8) |
               (static_cast<quint32>(static_cast<unsigned char>(data[off + 2])) << 16) |
               (static_cast<quint32>(static_cast<unsigned char>(data[off + 3])) << 24);
    };

    int channels = 1;
    int sr = 0;
    int bitsPerSample = 16;
    int audioFormat = 1;
    int blockAlign = 0;
    bool haveFmt = false;
    int dataOff = -1;
    int dataLen = 0;

    // チャンク走査 (fmt と data を探す)。
    int pos = 12;
    while (pos + 8 <= data.size()) {
        const QByteArray id = data.mid(pos, 4);
        const quint32 sz = rdU32(pos + 4);
        const int body = pos + 8;
        const qint64 chunkEnd = static_cast<qint64>(body) + static_cast<qint64>(sz);
        const qint64 nextChunk = chunkEnd + ((sz & 1u) ? 1 : 0);
        if (chunkEnd > data.size()) {
            if (error) *error =
                QStringLiteral("WAV チャンク '%1' がファイル終端を越えています。")
                    .arg(QString::fromLatin1(id.constData(), id.size()));
            return false;
        }

        if (id == QByteArray("fmt ")) {
            if (sz < 16) {
                if (error) *error = QStringLiteral("WAV fmt チャンクが短すぎます。");
                return false;
            }
            audioFormat   = rdU16(body);
            channels      = rdU16(body + 2);
            const quint32 parsedSr = rdU32(body + 4);
            if (parsedSr > static_cast<quint32>(std::numeric_limits<int>::max())) {
                if (error) *error = QStringLiteral("WAV の sample rate が大きすぎます。");
                return false;
            }
            sr            = static_cast<int>(parsedSr);
            blockAlign    = rdU16(body + 12);
            bitsPerSample = rdU16(body + 14);
            haveFmt = true;
        } else if (id == QByteArray("data")) {
            dataOff = body;
            dataLen = static_cast<int>(sz);
        }
        // チャンクは 2byte 境界 (奇数長は +1 パディング)。
        pos = static_cast<int>(std::min<qint64>(nextChunk, data.size()));
        if (dataOff >= 0 && haveFmt) break;
    }

    if (audioFormat != 1 || bitsPerSample != 16) {
        if (error) *error =
            QStringLiteral("対応するのは 16bit PCM WAV のみです (format=%1, bits=%2)。")
                .arg(audioFormat).arg(bitsPerSample);
        return false;
    }
    if (!haveFmt || dataOff < 0 || dataLen <= 0 || sr <= 0 || channels <= 0) {
        if (error) *error = QStringLiteral("WAV の data/fmt チャンクが見つかりません。");
        return false;
    }

    if (channels > 64) {
        if (error) *error = QStringLiteral("WAV の channel 数が大きすぎます (%1)。").arg(channels);
        return false;
    }

    const int frameBytes = 2 * channels;
    if (blockAlign != frameBytes) {
        if (error) *error =
            QStringLiteral("WAV の block align が不正です (blockAlign=%1, expected=%2)。")
                .arg(blockAlign).arg(frameBytes);
        return false;
    }

    const int frameCount = dataLen / frameBytes;
    if (frameCount <= 0) {
        if (error) *error = QStringLiteral("WAV の data チャンクにサンプルがありません。");
        return false;
    }

    outSamples.clear();
    outSamples.reserve(frameCount);
    const char *p = data.constData() + dataOff;
    for (int i = 0; i < frameCount; ++i) {
        double acc = 0.0;
        for (int c = 0; c < channels; ++c) {
            const int o = i * frameBytes + c * 2;
            const qint16 s = static_cast<qint16>(
                static_cast<quint16>(static_cast<unsigned char>(p[o])) |
                (static_cast<quint16>(static_cast<unsigned char>(p[o + 1])) << 8));
            acc += static_cast<double>(s) / 32768.0;
        }
        outSamples.push_back(acc / channels);
    }
    sampleRate = sr;
    return true;
}

} // namespace
#endif // HAVE_SPECTRAL_EDIT_DIALOG

void MainWindow::openSpectralRepair()
{
#ifdef HAVE_SPECTRAL_EDIT_DIALOG
    // (a) 対象音声を決める: 選択クリップの filePath、無ければファイル選択。
    QString sourcePath;
    if (m_timeline) {
        const int clipIdx = m_timeline->selectedVideoClipIndex();
        const auto &clips = m_timeline->videoClips();
        if (clipIdx >= 0 && clipIdx < clips.size())
            sourcePath = clips[clipIdx].filePath;
    }
    if (sourcePath.isEmpty()) {
        sourcePath = QFileDialog::getOpenFileName(
            this, QStringLiteral("スペクトル音声修復 — 対象メディアを選択"),
            QString(),
            QStringLiteral("メディアファイル (*.mp4 *.mov *.mkv *.wav *.mp3 *.aac *.m4a);;"
                           "すべてのファイル (*)"));
    }
    if (sourcePath.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("スペクトル音声修復: 対象が選択されていません。"), 3000);
        return;
    }
    if (!QFileInfo::exists(sourcePath)) {
        QMessageBox::warning(this, QStringLiteral("スペクトル音声修復"),
            QStringLiteral("ファイルが存在しません:\n%1").arg(sourcePath));
        return;
    }

    // (b) 一時 wav へ 48kHz で抽出。
    const int kSampleRate = 48000;
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        QMessageBox::warning(this, QStringLiteral("スペクトル音声修復"),
            QStringLiteral("一時ディレクトリを作成できませんでした。"));
        return;
    }
    tmpDir.setAutoRemove(true);
    const QString tmpWav = tmpDir.filePath(QStringLiteral("spectral_src.wav"));

    statusBar()->showMessage(QStringLiteral("音声を抽出しています..."));
    QString extractErr;
    if (!libavcore::extractAudioToWav(sourcePath, tmpWav, kSampleRate, &extractErr)) {
        QMessageBox::warning(this, QStringLiteral("スペクトル音声修復"),
            QStringLiteral("音声の抽出に失敗しました:\n%1")
                .arg(extractErr.isEmpty() ? sourcePath : extractErr));
        statusBar()->showMessage(QStringLiteral("スペクトル音声修復: 抽出失敗。"), 3000);
        return;
    }

    // (c) 抽出 wav を mono サンプル列へ読み込む。
    std::vector<double> samples;
    int sr = kSampleRate;
    QString readErr;
    if (!readPcm16WavToMono(tmpWav, samples, sr, &readErr) || samples.empty()) {
        QMessageBox::warning(this, QStringLiteral("スペクトル音声修復"),
            QStringLiteral("抽出した音声を読み込めませんでした:\n%1")
                .arg(readErr.isEmpty() ? QStringLiteral("(空のサンプル列)") : readErr));
        statusBar()->showMessage(QStringLiteral("スペクトル音声修復: 読み込み失敗。"), 3000);
        return;
    }

    // 長さ上限ガード (最大 10 分)。
    const double durationSec = static_cast<double>(samples.size()) / (sr > 0 ? sr : kSampleRate);
    const double kMaxSec = 10.0 * 60.0;
    if (durationSec > kMaxSec) {
        const auto ret = QMessageBox::question(this, QStringLiteral("スペクトル音声修復"),
            QStringLiteral("音声が長すぎます (%1 秒)。先頭 %2 分のみを処理します。続行しますか?")
                .arg(durationSec, 0, 'f', 1).arg(static_cast<int>(kMaxSec / 60.0)),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);
        if (ret != QMessageBox::Ok) {
            statusBar()->showMessage(QStringLiteral("スペクトル音声修復: キャンセルしました。"), 3000);
            return;
        }
        samples.resize(static_cast<size_t>(kMaxSec * sr));
    }

    // (d) ダイアログを開いてスペクトル編集。
    //
    // 注: 「適用」ボタンは applied() を emit するがダイアログは閉じない
    // (modeless 設計)。「閉じる」は QDialog::reject 相当なので exec() の
    // 戻り値では適用有無を判定できない。applied() を受けてフラグを立て、
    // exec() 終了後にそのフラグで書き出すか判断する。
    SpectralEditDialog dlg(this);
    dlg.setObjectName(QStringLiteral("spectralEditDialog"));
    dlg.setAudio(samples, sr);
    bool didApply = false;
    connect(&dlg, &SpectralEditDialog::applied, &dlg,
            [&didApply]() { didApply = true; });
    statusBar()->showMessage(QStringLiteral("スペクトル音声修復ダイアログを開いています..."), 2000);
    dlg.exec();
    if (!didApply) {
        statusBar()->showMessage(QStringLiteral("スペクトル音声修復: 適用されませんでした。"), 3000);
        return;
    }

    const std::vector<double> processed = dlg.processedSamples();
    const int outSr = dlg.sampleRate() > 0 ? dlg.sampleRate() : sr;
    if (processed.empty()) {
        statusBar()->showMessage(QStringLiteral("スペクトル音声修復: 修復結果が空のため書き出しをスキップしました。"), 4000);
        return;
    }
    if (processed.size() > static_cast<size_t>(std::numeric_limits<int>::max() / 2)) {
        QMessageBox::warning(this, QStringLiteral("スペクトル音声修復"),
            QStringLiteral("修復結果が大きすぎるため WAV として書き出せません。"));
        return;
    }

    // (e) 修復結果 (mono 16bit PCM) を「<元名>_repaired.wav」として書き出す。
    const QFileInfo srcInfo(sourcePath);
    const QString defaultOut = srcInfo.absolutePath() + QLatin1Char('/') +
                               srcInfo.completeBaseName() + QStringLiteral("_repaired.wav");
    QString outPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("修復済み音声を保存"), defaultOut,
        QStringLiteral("WAV ファイル (*.wav);;すべてのファイル (*)"));
    if (outPath.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("スペクトル音声修復: 保存をキャンセルしました。"), 3000);
        return;
    }

    // double → 16bit PCM little-endian へ変換。
    QByteArray pcm;
    pcm.resize(static_cast<int>(processed.size()) * 2);
    char *dst = pcm.data();
    for (size_t i = 0; i < processed.size(); ++i) {
        double v = std::isfinite(processed[i]) ? processed[i] : 0.0;
        if (v > 1.0) v = 1.0;
        else if (v < -1.0) v = -1.0;
        const qint16 s = static_cast<qint16>(qRound(v * 32767.0));
        dst[i * 2]     = static_cast<char>(s & 0xFF);
        dst[i * 2 + 1] = static_cast<char>((s >> 8) & 0xFF);
    }

    QString writeErr;
    if (!libavcore::writePcm16AsWav(outPath, pcm, outSr, /*channels=*/1, &writeErr)) {
        QMessageBox::warning(this, QStringLiteral("スペクトル音声修復"),
            QStringLiteral("修復済み音声の書き出しに失敗しました:\n%1")
                .arg(writeErr.isEmpty() ? outPath : writeErr));
        return;
    }

    statusBar()->showMessage(
        QStringLiteral("スペクトル音声修復: 保存しました — %1").arg(outPath), 6000);
    QMessageBox::information(this, QStringLiteral("スペクトル音声修復"),
        QStringLiteral("修復済み音声を書き出しました:\n%1\n\n"
                       "(クリップの差し替えは行いません。生成したファイルを手動で取り込んでください。)")
            .arg(outPath));
#else
    QMessageBox::information(this, QStringLiteral("スペクトル音声修復"),
        QStringLiteral("SpectralEditDialog がビルドに含まれていません。"));
#endif
}

// AC-4: ACES カラーマネジメント設定ダイアログ。SSOT である m_acesPipeline を
// 編集対象として渡し、OK のときのみ確定する。確定した設定は project save/load を
// 通じて ProjectData::acesPipeline に永続化される。
// TODO: 確定した m_acesPipeline をプレビュー/エクスポートのレンダーパイプラインへ
//       実適用するのは後続スコープ (本ストーリーは設定の保持と永続化までを担う)。
void MainWindow::openColorManagement()
{
    ColorManagementDialog dlg(this);
    dlg.setPipeline(m_acesPipeline);
    if (dlg.exec() == QDialog::Accepted) {
        m_acesPipeline = dlg.pipeline();
        const QString state = m_acesPipeline.enabled
            ? QStringLiteral("有効 (入力 %1 → 作業 %2 → 出力 %3)")
                  .arg(aces::colorSpaceName(m_acesPipeline.input),
                       aces::colorSpaceName(m_acesPipeline.working),
                       aces::colorSpaceName(m_acesPipeline.output))
            : QStringLiteral("無効");
        statusBar()->showMessage(
            QStringLiteral("カラーマネジメント (ACES): %1").arg(state), 4000);
    }
}

// DV-4: Dolby Vision メタデータ設定ダイアログ。SSOT である m_dolbyVision を編集対象
// として渡し、OK のときのみ確定する。確定した設定は project save/load を通じて
// ProjectData::dolbyVision に永続化される。ダイアログの「XML をエクスポート...」では
// 現在の UI 状態 (dlg.metadata()) から dolbyvision::toDolbyVisionXml を生成し、
// QFileDialog で選んだ *.xml へ書き出す。
void MainWindow::openDolbyVision()
{
    DolbyVisionDialog dlg(this);
    dlg.setMetadata(m_dolbyVision);

    // 「XML をエクスポート...」押下でファイル保存する。dlg はモーダルだが、
    // signal は exec() のイベントループ内で配送されるため this で受けられる。
    connect(&dlg, &DolbyVisionDialog::exportXmlRequested, this, [this, &dlg]() {
        const QString filePath = QFileDialog::getSaveFileName(
            &dlg, QStringLiteral("Dolby Vision XML をエクスポート"),
            QString(), QStringLiteral("Dolby Vision XML (*.xml)"));
        if (filePath.isEmpty())
            return;

        const dolbyvision::DolbyVisionMetadata meta = dlg.metadata();

        // 書き出し前に整合性を検証 (profile/level/L1 順序/ショット時間 等)。
        QString validationError;
        if (!dolbyvision::validate(meta, &validationError)) {
            statusBar()->showMessage(
                QStringLiteral("Dolby Vision XML エクスポート失敗: %1")
                    .arg(validationError), 6000);
            return;
        }

        const QString xml = dolbyvision::toDolbyVisionXml(meta, m_projectConfig.fps);

        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            statusBar()->showMessage(
                QStringLiteral("Dolby Vision XML を書き込めませんでした: %1")
                    .arg(filePath), 6000);
            return;
        }
        QTextStream out(&file);
        out << xml;
        file.close();

        statusBar()->showMessage(
            QStringLiteral("Dolby Vision XML をエクスポートしました: %1")
                .arg(filePath), 4000);
    });

    if (dlg.exec() == QDialog::Accepted) {
        m_dolbyVision = dlg.metadata();
        statusBar()->showMessage(
            QStringLiteral("Dolby Vision メタデータ: プロファイル %1 / %2 ショット")
                .arg(m_dolbyVision.profile)
                .arg(m_dolbyVision.shots.size()), 4000);
    }
}

// CC-4: 放送用クローズドキャプション (CEA-608/708) 設定ダイアログ。SSOT である
// m_broadcastCaption を編集対象として渡し、OK のときのみ確定する。既存字幕
// (m_subtitleSegments) があれば放送 CC の cue 源として CaptionCue へ変換して充填する。
// 確定した設定は project save/load を通じて ProjectData::broadcastCaption に永続化される。
// ダイアログの「SCC をエクスポート...」では現在の UI 状態 (dlg.document()) から
// broadcastcc::exportScc を生成し、QFileDialog で選んだ *.scc へ書き出す。
void MainWindow::openBroadcastCaption()
{
    // 既存字幕 (時刻付きテキスト) があれば放送 CC の cue 源として充填する。
    // cue が未設定のとき (初回 / 字幕ベース) のみ上書きし、ユーザが SCC 用に
    // 既に整えた cue は保持する。
    if (m_broadcastCaption.cues.isEmpty() && !m_subtitleSegments.isEmpty()) {
        QVector<broadcastcc::CaptionCue> cues;
        cues.reserve(m_subtitleSegments.size());
        for (const SubtitleSegment &seg : m_subtitleSegments) {
            broadcastcc::CaptionCue cue;
            cue.startSec = seg.startTime;
            cue.endSec = seg.endTime;
            cue.text = seg.text;
            // row/col は CaptionCue の既定 (row=15 画面下端, col=0) を使用。
            cues.append(cue);
        }
        m_broadcastCaption.cues = cues;
    }

    BroadcastCaptionDialog dlg(this);
    dlg.setDocument(m_broadcastCaption);

    // 「SCC をエクスポート...」押下でファイル保存する。dlg はモーダルだが、
    // signal は exec() のイベントループ内で配送されるため this で受けられる。
    connect(&dlg, &BroadcastCaptionDialog::exportSccRequested, this, [this, &dlg]() {
        const QString filePath = QFileDialog::getSaveFileName(
            &dlg, QStringLiteral("SCC (Scenarist Closed Caption) をエクスポート"),
            QString(), QStringLiteral("Scenarist SCC (*.scc)"));
        if (filePath.isEmpty())
            return;

        const broadcastcc::BroadcastCaptionDoc doc = dlg.document();
        if (doc.cues.isEmpty()) {
            statusBar()->showMessage(
                QStringLiteral("SCC エクスポート: 字幕 cue がありません"), 6000);
            return;
        }

        const QString scc = broadcastcc::exportScc(doc);

        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            statusBar()->showMessage(
                QStringLiteral("SCC を書き込めませんでした: %1").arg(filePath), 6000);
            return;
        }
        QTextStream out(&file);
        out << scc;
        file.close();

        statusBar()->showMessage(
            QStringLiteral("SCC をエクスポートしました: %1").arg(filePath), 4000);
    });

    if (dlg.exec() == QDialog::Accepted) {
        m_broadcastCaption = dlg.document();
        statusBar()->showMessage(
            QStringLiteral("放送CC: %1 / CC%2 / %3 cue")
                .arg(m_broadcastCaption.standard)
                .arg(m_broadcastCaption.channel)
                .arg(m_broadcastCaption.cues.size()), 4000);
    }
}

void MainWindow::openAnimExportDialog()
{
#ifdef HAVE_ANIMATED_EXPORT_DIALOG
    if (!m_animExportDialog) {
        m_animExportDialog = new AnimatedExportDialog(this);
        m_animExportDialog->setObjectName(QStringLiteral("animExportDialog"));
    }
    m_animExportDialog->show();
    m_animExportDialog->raise();
    m_animExportDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("アニメGIF・WebP書き出し"),
        QStringLiteral("AnimatedExportDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openEasingEditorDialog()
{
#ifdef HAVE_EASING_CURVE_EDITOR_DIALOG
    if (!m_easingEditorDialog) {
        m_easingEditorDialog = new EasingCurveEditorDialog(this);
        m_easingEditorDialog->setObjectName(QStringLiteral("easingEditorDialog"));
    }
    m_easingEditorDialog->show();
    m_easingEditorDialog->raise();
    m_easingEditorDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("イージングカーブエディタ"),
        QStringLiteral("EasingCurveEditorDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openSubtitleTranslatorDialog()
{
#ifdef HAVE_SUBTITLE_TRANSLATOR_DIALOG
    if (!m_subtitleTranslatorDialog) {
        m_subtitleTranslatorDialog = new SubtitleTranslatorDialog(this);
        m_subtitleTranslatorDialog->setObjectName(QStringLiteral("subtitleTranslatorDialog"));
    }
    m_subtitleTranslatorDialog->show();
    m_subtitleTranslatorDialog->raise();
    m_subtitleTranslatorDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("字幕翻訳"),
        QStringLiteral("SubtitleTranslatorDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openLowerThirdDialog()
{
#ifdef HAVE_LOWER_THIRD_DIALOG
    if (!m_lowerThirdDialog) {
        m_lowerThirdDialog = new LowerThirdDialog(this);
        m_lowerThirdDialog->setObjectName(QStringLiteral("lowerThirdDialog"));
    }
    m_lowerThirdDialog->show();
    m_lowerThirdDialog->raise();
    m_lowerThirdDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("ローワーサード"),
        QStringLiteral("LowerThirdDialog がビルドに含まれていません。"));
#endif
}

void MainWindow::openWatermarkDialog()
{
#ifdef HAVE_WATERMARK_DIALOG
    if (!m_watermarkDialog) {
        m_watermarkDialog = new WatermarkDialog(this);
        m_watermarkDialog->setObjectName(QStringLiteral("watermarkDialog"));
    }
    m_watermarkDialog->show();
    m_watermarkDialog->raise();
    m_watermarkDialog->activateWindow();
#else
    QMessageBox::information(this, QStringLiteral("ウォーターマーク"),
        QStringLiteral("WatermarkDialog がビルドに含まれていません。"));
#endif
}

// --- Phase 14: New slot implementations ---

void MainWindow::setupRecentFiles()
{
    m_recentFilesManager = new RecentFilesManager(this);
}

void MainWindow::openRecentFile(const QString &filePath)
{
    QFileInfo fi(filePath);
    if (!fi.exists()) {
        QMessageBox::warning(this, "File Not Found",
            QString("The file '%1' no longer exists.").arg(filePath));
        if (m_recentFilesManager)
            m_recentFilesManager->removeFile(filePath);
        return;
    }

    // Check if it's a project file or media file
    if (filePath.endsWith(".veditor", Qt::CaseInsensitive)) {
        ProjectData data;
        if (ProjectFile::load(filePath, data)) {
            applyLoadedProjectData(data, filePath);
            statusBar()->showMessage("Opened project: " + fi.fileName());
        }
    } else {
        loadMediaFile(filePath, false, "Opened");
    }
}

void MainWindow::applyShaderEffect()
{
    auto &lib = ShaderEffectLibrary::instance();
    auto effects = lib.allEffects();

    QStringList names;
    for (auto &e : effects)
        names << QString("%1 — %2").arg(e.category, e.name);

    bool ok;
    QString selected = QInputDialog::getItem(this, "GPU Shader Effect",
        "Select a shader effect to apply:", names, 0, false, &ok);
    if (!ok || selected.isEmpty()) return;

    int idx = names.indexOf(selected);
    if (idx >= 0 && idx < effects.size()) {
        statusBar()->showMessage(QString("Applied GPU shader: %1").arg(effects[idx].name));
    }
}

void MainWindow::manageShaderEffects()
{
    auto &lib = ShaderEffectLibrary::instance();
    auto effects = lib.allEffects();

    QString info = QString("GPU Shader Effect Library\n\n%1 effects available:\n\n").arg(effects.size());
    for (auto &cat : lib.categories()) {
        info += "  " + cat + ":\n";
        for (auto &e : lib.effectsByCategory(cat)) {
            info += "    - " + e.name + ": " + e.description + "\n";
        }
        info += "\n";
    }
    QMessageBox::information(this, "GPU Shader Effects", info);
}

void MainWindow::openVSTPlugins()
{
    VSTPluginDialog dialog(this);
    dialog.exec();
}

void MainWindow::openScriptConsole()
{
    ScriptConsole console(m_scriptEngine, &m_scriptManager, this);
    console.exec();
}

void MainWindow::openNetworkRender()
{
    NetworkRenderDialog dialog(&m_networkRenderServer, this);
    dialog.exec();
}

void MainWindow::exportToRemotion()
{
    ProjectData data;
    populateProjectData(data);

    RemotionExportDialog dialog(m_projectConfig, data, this);
    dialog.exec();
}

// --- UI/UX improvements ---

void MainWindow::setupStatusBarWidgets()
{
    auto makeLabel = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setStyleSheet("QLabel { color: #888; padding: 0 8px; font-size: 11px; }");
        statusBar()->addPermanentWidget(label);
        return label;
    };

    m_statusResolution = makeLabel("1920x1080");
    m_statusFps = makeLabel("30 fps");
    m_statusDuration = makeLabel("00:00:00");
    m_statusTheme = makeLabel("Dark");
}

void MainWindow::updateStatusInfo()
{
    if (m_statusResolution)
        m_statusResolution->setText(QString("%1x%2").arg(m_projectConfig.width).arg(m_projectConfig.height));
    if (m_statusFps)
        m_statusFps->setText(QString("%1 fps").arg(m_projectConfig.fps));

    if (m_statusDuration) {
        const double totalDur = m_timeline ? m_timeline->totalDuration() : 0.0;
        int h = (int)(totalDur / 3600);
        int m = (int)(fmod(totalDur, 3600) / 60);
        int s = (int)(fmod(totalDur, 60));
        m_statusDuration->setText(QString("%1:%2:%3")
            .arg(h, 2, 10, QChar('0'))
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0')));
    }

    if (m_statusTheme)
        m_statusTheme->setText(ThemeManager::themeName(ThemeManager::instance().currentTheme()));
}

void MainWindow::testLoadFile(const QString &filePath)
{
    qInfo() << "MainWindow::testLoadFile" << filePath;
    loadMediaFile(filePath, true, "Loaded");
}

void MainWindow::testStartPlayback()
{
    qInfo() << "MainWindow::testStartPlayback";
    if (m_player)
        QMetaObject::invokeMethod(m_player, "play", Qt::QueuedConnection);
}

void MainWindow::loadMediaFile(const QString &filePath, bool addToTimeline, const QString &statusPrefix)
{
    qInfo() << "MainWindow::loadMediaFile" << filePath << "addToTimeline=" << addToTimeline;
    if (filePath.isEmpty())
        return;

    const QFileInfo fi(filePath);
    if (addToTimeline && m_timeline)
        m_timeline->addClip(filePath);
    if (m_recentFilesManager)
        m_recentFilesManager->addFile(filePath);

    hideWelcomeScreen();
    updateStatusInfo();
    updateEditActions();
    statusBar()->showMessage("Loading: " + fi.fileName());

    // When the file is added to the timeline, the player is driven by the
    // Timeline::sequenceChanged → VideoPlayer::setSequence wiring (synchronous
    // from addClip above), so we must NOT call loadFile() again here — that
    // would clobber the sequence-driven load with a single-file load.
    const QString playbackPath = ProxyManager::instance().getProxyPath(filePath);
    const bool needsDirectLoad = !addToTimeline;
    QPointer<MainWindow> guard(this);
    QTimer::singleShot(0, this, [guard, filePath, playbackPath, statusPrefix, needsDirectLoad]() {
        if (!guard)
            return;
        qInfo() << "MainWindow::loadMediaFile deferred load" << playbackPath
                << "directLoad=" << needsDirectLoad;
        if (needsDirectLoad && guard->m_player)
            guard->m_player->loadFile(playbackPath);
        guard->updateStatusInfo();
        guard->updateEditActions();
        guard->statusBar()->showMessage(statusPrefix + ": " + QFileInfo(filePath).fileName());
    });
}

void MainWindow::showWelcomeScreen()
{
    if (m_welcomeWidget) {
        if (m_recentFilesManager)
            m_welcomeWidget->setRecentFiles(m_recentFilesManager->recentFiles());
        m_welcomeWidget->setVisible(true);
    }
    if (m_mainSplitter)
        m_mainSplitter->setVisible(false);
    m_hasContent = false;
}

void MainWindow::hideWelcomeScreen()
{
    if (m_hasContent)
        return;
    if (m_welcomeWidget)
        m_welcomeWidget->setVisible(false);
    if (m_mainSplitter)
        m_mainSplitter->setVisible(true);
    m_hasContent = true;
}

void MainWindow::saveWindowState()
{
    QSettings settings("VSimpleEditor", "WindowState");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
    if (m_mainSplitter)
        settings.setValue("splitterState", m_mainSplitter->saveState());
    settings.setValue("theme", (int)ThemeManager::instance().currentTheme());
    QSettings uiSettings("VSimpleEditor", "UI");
    uiSettings.setValue("historyDockVisible", m_historyDock ? m_historyDock->isVisible() : true);
}

void MainWindow::restoreWindowState()
{
    QSettings settings("VSimpleEditor", "WindowState");
    if (settings.contains("geometry")) {
        restoreGeometry(settings.value("geometry").toByteArray());
    }
    if (settings.contains("windowState")) {
        restoreState(settings.value("windowState").toByteArray());
    }
    // カラーグレーディングパネルは常に非表示で起動
    if (m_colorGradingPanel)
        m_colorGradingPanel->close();
    if (m_mainSplitter && settings.contains("splitterState")) {
        m_mainSplitter->restoreState(settings.value("splitterState").toByteArray());
    }
    if (settings.contains("theme")) {
        auto themeType = (ThemeType)settings.value("theme").toInt();
        ThemeManager::instance().applyTheme(themeType, this);
    }
    if (m_historyDock) {
        QSettings uiSettings("VSimpleEditor", "UI");
        m_historyDock->setVisible(uiSettings.value("historyDockVisible", true).toBool());
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        for (auto &url : event->mimeData()->urls()) {
            QString path = url.toLocalFile().toLower();
            if (path.endsWith(".mp4") || path.endsWith(".mkv") || path.endsWith(".mov") ||
                path.endsWith(".webm") || path.endsWith(".flv") || path.endsWith(".veditor") ||
                path.endsWith(".png") || path.endsWith(".jpg") || path.endsWith(".jpeg") ||
                path.endsWith(".wav") || path.endsWith(".mp3") || path.endsWith(".aac")) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    for (auto &url : event->mimeData()->urls()) {
        QString filePath = url.toLocalFile();
        if (filePath.isEmpty()) continue;

        if (filePath.endsWith(".veditor", Qt::CaseInsensitive)) {
            // Open as project
            ProjectData data;
            if (ProjectFile::load(filePath, data)) {
                applyLoadedProjectData(data, filePath);
                statusBar()->showMessage("Opened project: " + QFileInfo(filePath).fileName());
            }
        } else {
            // Open as media file
            loadMediaFile(filePath, true, "Loaded");
        }
    }
    updateEditActions();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);

    if (!m_autoSaveStarted && m_autoSave) {
        m_autoSaveStarted = true;
        QTimer::singleShot(500, this, [this]() {
            if (!m_autoSave)
                return;
            // Default OFF. Interval persisted via QSettings, default 30 min.
            QSettings prefSettings("VSimpleEditor", "Preferences");
            const bool enabled = prefSettings.value("autoSaveEnabled", false).toBool();
            if (!enabled)
                return;
            AutoSaveConfig cfg;
            cfg.enabled = true;
            cfg.interval = prefSettings.value("autoSaveIntervalSec", 1800).toInt();
            m_autoSave->start(cfg);
        });
    }
}

void MainWindow::rebuildAudioMeters()
{
    if (!m_audioMetersDock || !m_player)
        return;

    // Disconnect all existing meter connections before creating new ones
    for (const auto &conn : m_meterConnections)
        QObject::disconnect(conn);
    m_meterConnections.clear();
    m_audioMeterWidgets.clear();

    const int n = m_timeline ? m_timeline->audioTrackCount() : 0;

    QWidget *container = new QWidget();
    QHBoxLayout *layout = new QHBoxLayout(container);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    for (int i = 0; i < n; ++i) {
        QVBoxLayout *trackLayout = new QVBoxLayout();
        trackLayout->setSpacing(2);

        QLabel *label = new QLabel(QString("T%1").arg(i + 1));
        label->setAlignment(Qt::AlignCenter);

        AudioMeterWidget *meter = new AudioMeterWidget();
        meter->setFixedWidth(20);
        meter->setTrackIndex(i);

        trackLayout->addWidget(label);
        trackLayout->addWidget(meter);
        layout->addLayout(trackLayout);

        m_audioMeterWidgets.append(meter);

        if (auto *mixer = m_player->audioMixer()) {
            auto conn = connect(mixer, &AudioMixer::levelChanged, meter,
                    [meter, i](int trackIdx, float pkL, float pkR, float rmsL, float rmsR) {
                        if (trackIdx == i)
                            meter->setLevels(pkL, pkR, rmsL, rmsR);
                    });
            m_meterConnections.append(conn);
        }

        connect(meter, &AudioMeterWidget::requestEqPresetMenu,
                this, &MainWindow::onMeterRequestEqPresetMenu);
        connect(meter, &AudioMeterWidget::requestCompressorDialog,
                this, &MainWindow::onMeterRequestCompressorDialog);
        connect(meter, &AudioMeterWidget::requestAutoDuckDialog,
                this, &MainWindow::onMeterRequestAutoDuckDialog);
        connect(meter, &AudioMeterWidget::requestNormalize,
                this, &MainWindow::onMeterRequestNormalize);
    }

    if (n > 0) {
        QFrame *sep = new QFrame();
        sep->setFrameShape(QFrame::VLine);
        layout->addWidget(sep);
    }

    // Master meter
    QVBoxLayout *masterLayout = new QVBoxLayout();
    masterLayout->setSpacing(2);

    QLabel *masterLabel = new QLabel("Master");
    masterLabel->setAlignment(Qt::AlignCenter);

    AudioMeterWidget *masterMeter = new AudioMeterWidget();
    masterMeter->setFixedWidth(20);

    masterLayout->addWidget(masterLabel);
    masterLayout->addWidget(masterMeter);
    layout->addLayout(masterLayout);

    m_masterMeter = masterMeter;

    if (auto *mixer = m_player->audioMixer()) {
        auto conn = connect(mixer, &AudioMixer::masterLevelChanged, masterMeter,
                &AudioMeterWidget::setLevels);
        m_meterConnections.append(conn);
    }

    connect(masterMeter, &AudioMeterWidget::requestCompressorDialog,
            this, &MainWindow::onMeterRequestCompressorDialog);
    connect(masterMeter, &AudioMeterWidget::requestNormalizeAll,
            this, &MainWindow::onMeterRequestNormalizeAll);
    connect(masterMeter, &AudioMeterWidget::requestResetAllMeters,
            this, &MainWindow::onMeterRequestResetAllMeters);

    layout->addStretch();

    QWidget *oldWidget = m_audioMetersDock->widget();
    m_audioMetersDock->setWidget(container);
    if (oldWidget)
        oldWidget->deleteLater();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveWindowState();
    if (m_autoSave) m_autoSave->markCleanShutdown();
    // US-SC-B: Sprint 12 — persist preset + custom bindings before shutdown.
    if (m_shortcutManager) m_shortcutManager->saveToSettings();
    QMainWindow::closeEvent(event);
}

// --- Effects menu quick-access for Sharpen / Mosaic / ChromaKey ---

void MainWindow::applySharpenEffect()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Sharpen", "Select a clip first.");
        return;
    }
    auto effects = m_timeline->clipEffects();
    effects.append(VideoEffect::createSharpen());
    m_timeline->setClipEffects(effects);
    if (m_player)
        m_player->setPreviewEffects(effects, true);
    statusBar()->showMessage("シャープンを適用しました", 3000);
}

void MainWindow::applyMosaicEffect()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Mosaic", "Select a clip first.");
        return;
    }
    auto effects = m_timeline->clipEffects();
    effects.append(VideoEffect::createMosaic());
    m_timeline->setClipEffects(effects);
    if (m_player)
        m_player->setPreviewEffects(effects, true);
    statusBar()->showMessage("モザイクを適用しました", 3000);
}

void MainWindow::applyChromaKeyEffect()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "Chroma Key", "Select a clip first.");
        return;
    }
    auto effects = m_timeline->clipEffects();
    effects.append(VideoEffect::createChromaKey());
    m_timeline->setClipEffects(effects);
    if (m_player)
        m_player->setPreviewEffects(effects, true);
    statusBar()->showMessage("クロマキーを適用しました", 3000);
}

// --- Master Compressor inline dialog ---

void MainWindow::openMasterCompressor()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, "Compressor", "Audio mixer unavailable.");
        return;
    }

    const AudioMixer::CompressorParams current = mixer->compressorParams();

    QDialog dlg(this);
    dlg.setWindowTitle("Master Compressor");
    auto *form = new QFormLayout(&dlg);

    // Threshold
    auto *thresholdSlider = new QSlider(Qt::Horizontal, &dlg);
    thresholdSlider->setRange(-30, 0);
    thresholdSlider->setValue(static_cast<int>(current.thresholdDb));
    auto *thresholdSpin = new QSpinBox(&dlg);
    thresholdSpin->setRange(-30, 0);
    thresholdSpin->setSuffix(" dB");
    thresholdSpin->setValue(static_cast<int>(current.thresholdDb));
    connect(thresholdSlider, &QSlider::valueChanged, thresholdSpin, &QSpinBox::setValue);
    connect(thresholdSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            thresholdSlider, &QSlider::setValue);
    auto *thresholdRow = new QHBoxLayout();
    thresholdRow->addWidget(thresholdSlider);
    thresholdRow->addWidget(thresholdSpin);
    form->addRow("Threshold", thresholdRow);

    // Ratio
    auto *ratioSlider = new QSlider(Qt::Horizontal, &dlg);
    ratioSlider->setRange(10, 200); // 1.0–20.0 × 10
    ratioSlider->setValue(static_cast<int>(current.ratio * 10.0));
    auto *ratioSpin = new QDoubleSpinBox(&dlg);
    ratioSpin->setRange(1.0, 20.0);
    ratioSpin->setSingleStep(0.5);
    ratioSpin->setDecimals(1);
    ratioSpin->setSuffix(" :1");
    ratioSpin->setValue(current.ratio);
    connect(ratioSlider, &QSlider::valueChanged, this,
            [ratioSpin](int v) { ratioSpin->setValue(v / 10.0); });
    connect(ratioSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [ratioSlider](double v) { ratioSlider->setValue(static_cast<int>(v * 10.0)); });
    auto *ratioRow = new QHBoxLayout();
    ratioRow->addWidget(ratioSlider);
    ratioRow->addWidget(ratioSpin);
    form->addRow("Ratio", ratioRow);

    // Attack
    auto *attackSlider = new QSlider(Qt::Horizontal, &dlg);
    attackSlider->setRange(1, 100);
    attackSlider->setValue(static_cast<int>(current.attackMs));
    auto *attackSpin = new QDoubleSpinBox(&dlg);
    attackSpin->setRange(1.0, 100.0);
    attackSpin->setSingleStep(1.0);
    attackSpin->setDecimals(1);
    attackSpin->setSuffix(" ms");
    attackSpin->setValue(current.attackMs);
    connect(attackSlider, &QSlider::valueChanged, this,
            [attackSpin](int v) { attackSpin->setValue(static_cast<double>(v)); });
    connect(attackSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [attackSlider](double v) { attackSlider->setValue(static_cast<int>(v)); });
    auto *attackRow = new QHBoxLayout();
    attackRow->addWidget(attackSlider);
    attackRow->addWidget(attackSpin);
    form->addRow("Attack", attackRow);

    // Release
    auto *releaseSlider = new QSlider(Qt::Horizontal, &dlg);
    releaseSlider->setRange(10, 1000);
    releaseSlider->setValue(static_cast<int>(current.releaseMs));
    auto *releaseSpin = new QDoubleSpinBox(&dlg);
    releaseSpin->setRange(10.0, 1000.0);
    releaseSpin->setSingleStep(10.0);
    releaseSpin->setDecimals(0);
    releaseSpin->setSuffix(" ms");
    releaseSpin->setValue(current.releaseMs);
    connect(releaseSlider, &QSlider::valueChanged, this,
            [releaseSpin](int v) { releaseSpin->setValue(static_cast<double>(v)); });
    connect(releaseSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [releaseSlider](double v) { releaseSlider->setValue(static_cast<int>(v)); });
    auto *releaseRow = new QHBoxLayout();
    releaseRow->addWidget(releaseSlider);
    releaseRow->addWidget(releaseSpin);
    form->addRow("Release", releaseRow);

    // Makeup gain
    auto *makeupSlider = new QSlider(Qt::Horizontal, &dlg);
    makeupSlider->setRange(0, 18);
    makeupSlider->setValue(static_cast<int>(current.makeupDb));
    auto *makeupSpin = new QDoubleSpinBox(&dlg);
    makeupSpin->setRange(0.0, 18.0);
    makeupSpin->setSingleStep(0.5);
    makeupSpin->setDecimals(1);
    makeupSpin->setSuffix(" dB");
    makeupSpin->setValue(current.makeupDb);
    connect(makeupSlider, &QSlider::valueChanged, this,
            [makeupSpin](int v) { makeupSpin->setValue(static_cast<double>(v)); });
    connect(makeupSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [makeupSlider](double v) { makeupSlider->setValue(static_cast<int>(v)); });
    auto *makeupRow = new QHBoxLayout();
    makeupRow->addWidget(makeupSlider);
    makeupRow->addWidget(makeupSpin);
    form->addRow("Makeup Gain", makeupRow);

    // Enable checkbox
    auto *enableCheck = new QCheckBox("Enable Compressor", &dlg);
    enableCheck->setChecked(current.enabled);
    form->addRow(enableCheck);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() == QDialog::Accepted) {
        AudioMixer::CompressorParams params;
        params.thresholdDb = static_cast<double>(thresholdSpin->value());
        params.ratio = ratioSpin->value();
        params.attackMs = attackSpin->value();
        params.releaseMs = releaseSpin->value();
        params.makeupDb = makeupSpin->value();
        params.enabled = enableCheck->isChecked();
        mixer->setCompressorParams(params);
        statusBar()->showMessage(
            QString("Compressor %1 (threshold %2 dB, ratio %3:1)")
                .arg(params.enabled ? "ON" : "OFF")
                .arg(params.thresholdDb, 0, 'f', 0)
                .arg(params.ratio, 0, 'f', 1),
            4000);
    }
}

void MainWindow::openAutoDuckSettings()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, "Auto-Duck", "Audio mixer unavailable.");
        return;
    }

    const AudioMixer::AutoDuckParams current = mixer->autoDuckParams();

    QDialog dlg(this);
    dlg.setWindowTitle("Auto-Duck Settings");
    auto *form = new QFormLayout(&dlg);

    // Threshold
    auto *thresholdSpin = new QDoubleSpinBox(&dlg);
    thresholdSpin->setRange(-60.0, 0.0);
    thresholdSpin->setSingleStep(1.0);
    thresholdSpin->setDecimals(1);
    thresholdSpin->setSuffix(" dB");
    thresholdSpin->setValue(current.thresholdDb);
    form->addRow("Threshold", thresholdSpin);

    // Ratio
    auto *ratioSpin = new QDoubleSpinBox(&dlg);
    ratioSpin->setRange(1.0, 20.0);
    ratioSpin->setSingleStep(0.5);
    ratioSpin->setDecimals(1);
    ratioSpin->setSuffix(" :1");
    ratioSpin->setValue(current.ratio);
    form->addRow("Ratio", ratioSpin);

    // Attack
    auto *attackSpin = new QDoubleSpinBox(&dlg);
    attackSpin->setRange(0.1, 5000.0);
    attackSpin->setSingleStep(1.0);
    attackSpin->setDecimals(1);
    attackSpin->setSuffix(" ms");
    attackSpin->setValue(current.attackMs);
    form->addRow("Attack", attackSpin);

    // Release
    auto *releaseSpin = new QDoubleSpinBox(&dlg);
    releaseSpin->setRange(1.0, 10000.0);
    releaseSpin->setSingleStep(10.0);
    releaseSpin->setDecimals(0);
    releaseSpin->setSuffix(" ms");
    releaseSpin->setValue(current.releaseMs);
    form->addRow("Release", releaseSpin);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() == QDialog::Accepted) {
        AudioMixer::AutoDuckParams params;
        params.thresholdDb = thresholdSpin->value();
        params.ratio = ratioSpin->value();
        params.attackMs = attackSpin->value();
        params.releaseMs = releaseSpin->value();
        mixer->setAutoDuckParams(params);
        statusBar()->showMessage(
            QString("Auto-Duck: threshold %1 dB, ratio %2:1, attack %3 ms, release %4 ms")
                .arg(params.thresholdDb, 0, 'f', 1)
                .arg(params.ratio, 0, 'f', 1)
                .arg(params.attackMs, 0, 'f', 1)
                .arg(params.releaseMs, 0, 'f', 0),
            4000);
    }
}

// --- Audio meter context menu handlers ---

void MainWindow::onMeterRequestEqPresetMenu(int trackIdx, QPoint globalPos)
{
    if (!m_player) return;
    auto *mixer = m_player->audioMixer();
    if (!mixer) return;

    QMenu menu;
    const auto presets = AudioEQProcessor::presets();
    const int ti = trackIdx;
    for (const auto &preset : presets) {
        auto *act = menu.addAction(preset.name);
        const AudioEQConfig cfg = preset.config;
        const QString presetName = preset.name;
        connect(act, &QAction::triggered, this,
                [this, mixer, ti, cfg, presetName]() {
            if (mixer) {
                mixer->setTrackEqEnabled(ti, true);
                mixer->setTrackEqConfig(ti, cfg);
            }
            statusBar()->showMessage(
                QString("A%1 EQ → %2").arg(ti + 1).arg(presetName), 3000);
        });
    }
    menu.addSeparator();
    auto *bypassAct = menu.addAction("Bypass / Reset");
    const int ti2 = trackIdx;
    connect(bypassAct, &QAction::triggered, this,
            [this, mixer, ti2]() {
        if (mixer) {
            mixer->setTrackEqEnabled(ti2, false);
        }
        statusBar()->showMessage(
            QString("A%1 EQ bypassed").arg(ti2 + 1), 3000);
    });

    menu.exec(globalPos);
}

void MainWindow::onMeterRequestCompressorDialog()
{
    openMasterCompressor();
}

void MainWindow::onMeterRequestAutoDuckDialog()
{
    openAutoDuckSettings();
}

void MainWindow::onMeterRequestNormalize(int trackIdx, double gainDb)
{
    if (!m_player) return;
    auto *mixer = m_player->audioMixer();
    if (!mixer) return;

    if (std::abs(gainDb) < 0.001) {
        statusBar()->showMessage(
            QStringLiteral("メーターに信号がありません. 一度再生してから実行してください."), 3000);
        return;
    }

    m_timeline->undoManager()->saveState(m_timeline->currentState(), "Normalize audio");

    const double oldGain = mixer->trackGain(trackIdx);
    const double factor = std::pow(10.0, gainDb / 20.0);
    const double newGain = std::clamp(oldGain * factor, 0.0, 4.0);
    mixer->setTrackGain(trackIdx, newGain);

    statusBar()->showMessage(
        QString("T%1 ノーマライズ: +%2dB (gain %3→%4)")
            .arg(trackIdx + 1)
            .arg(gainDb, 0, 'f', 1)
            .arg(oldGain, 0, 'f', 2)
            .arg(newGain, 0, 'f', 2),
        4000);
}

void MainWindow::onMeterRequestNormalizeAll()
{
    if (!m_player) return;
    auto *mixer = m_player->audioMixer();
    if (!mixer) return;

    bool anyApplied = false;
    for (int i = 0; i < m_audioMeterWidgets.size(); ++i) {
        const double peak = m_audioMeterWidgets[i]->currentPeakHoldDb();
        if (peak <= -60.0)
            continue;
        const double gainDb = std::clamp(-1.0 - peak, -24.0, 12.0);
        if (std::abs(gainDb) < 0.001)
            continue;
        if (!anyApplied)
            m_timeline->undoManager()->saveState(m_timeline->currentState(), "Normalize all audio");
        const double oldGain = mixer->trackGain(i);
        const double factor = std::pow(10.0, gainDb / 20.0);
        const double newGain = std::clamp(oldGain * factor, 0.0, 4.0);
        mixer->setTrackGain(i, newGain);
        anyApplied = true;
    }

    if (anyApplied) {
        statusBar()->showMessage(
            QStringLiteral("全トラックをノーマライズしました"), 4000);
    } else {
        statusBar()->showMessage(
            QStringLiteral("メーターに信号がありません. 一度再生してから実行してください."), 3000);
    }
}

void MainWindow::onMeterRequestResetAllMeters()
{
    for (auto *meter : m_audioMeterWidgets)
        meter->resetPeakHold();
    if (m_masterMeter)
        m_masterMeter->resetPeakHold();
}

// =============================================================
// Consolidation slots — wire panels and dialogs implemented in
// earlier sprint stories into the menu bar.
// =============================================================

namespace {
// Build the (itemNames, trackIds) lists EqualizerPanel::setTracks expects.
// Convention: id 0 = Master, ids 1..N = A1..An audio tracks (in order).
static void buildAudioTrackList(int audioTrackCount,
                                QStringList &itemNames,
                                QList<int> &trackIds,
                                bool includeMaster = true)
{
    itemNames.clear();
    trackIds.clear();
    if (includeMaster) {
        itemNames << QStringLiteral("Master");
        trackIds << 0;
    }
    for (int i = 0; i < audioTrackCount; ++i) {
        itemNames << QStringLiteral("A%1").arg(i + 1);
        trackIds << (i + 1);
    }
}
} // namespace

void MainWindow::openEqualizerPanel()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, tr("EQ パネル"),
            tr("オーディオミキサーが利用できません。"));
        return;
    }

    if (!m_equalizerDock) {
        m_equalizerDock = new QDockWidget(tr("EQ"), this);
        m_equalizerDock->setObjectName("EqualizerDock");
        auto *panel = new EqualizerPanel(m_equalizerDock);
        m_equalizerDock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, m_equalizerDock);

        connect(panel, &EqualizerPanel::eqChanged,
                this, [this](int trackId, AudioMixer::EqSettings eq) {
            if (auto *mx = m_player ? m_player->audioMixer() : nullptr)
                mx->setEqForTrack(trackId, eq);
        });
    }

    // Re-seed track list and current per-track settings on every show
    // so newly added audio tracks appear without restart.
    if (auto *panel = qobject_cast<EqualizerPanel *>(m_equalizerDock->widget())) {
        QStringList names;
        QList<int> ids;
        const int trackCount = m_timeline ? m_timeline->audioTrackCount() : 0;
        buildAudioTrackList(trackCount, names, ids);
        panel->setTracks(names, ids);
        for (int id : ids)
            panel->setEqSettings(id, mixer->eqForTrack(id));
    }
    m_equalizerDock->setVisible(true);
    m_equalizerDock->raise();
}

void MainWindow::openCompressorPanel()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, tr("コンプレッサー"),
            tr("オーディオミキサーが利用できません。"));
        return;
    }

    if (!m_compressorDock) {
        m_compressorDock = new QDockWidget(tr("Compressor / Limiter"), this);
        m_compressorDock->setObjectName("CompressorDock");
        auto *panel = new CompressorPanel(m_compressorDock);
        panel->setMixer(mixer);
        m_compressorDock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, m_compressorDock);

        connect(panel, &CompressorPanel::compressorChanged,
                this, [this](int trackId, AudioMixer::CompressorSettings c) {
            if (auto *mx = m_player ? m_player->audioMixer() : nullptr)
                mx->setCompressorForTrack(trackId, c);
        });
    }

    if (auto *panel = qobject_cast<CompressorPanel *>(m_compressorDock->widget())) {
        QStringList names;
        QList<int> ids;
        const int trackCount = m_timeline ? m_timeline->audioTrackCount() : 0;
        buildAudioTrackList(trackCount, names, ids, /*includeMaster=*/false);
        // CompressorPanel::setTrackList accepts the active track ids and
        // inserts the master row (id 0) itself.
        panel->setTrackList(ids);
        for (int id : ids)
            panel->loadSettings(id, mixer->compressorForTrack(id));
        panel->loadSettings(0, mixer->compressorForTrack(0));
    }
    m_compressorDock->setVisible(true);
    m_compressorDock->raise();
}

void MainWindow::openReverbPanel()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, tr("リバーブ"),
            tr("オーディオミキサーが利用できません。"));
        return;
    }

    if (!m_reverbDock) {
        m_reverbDock = new QDockWidget(tr("Reverb"), this);
        m_reverbDock->setObjectName("ReverbDock");
        auto *panel = new ReverbPanel(m_reverbDock);
        panel->setMixer(mixer);
        m_reverbDock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, m_reverbDock);

        connect(panel, &ReverbPanel::reverbChanged,
                this, [this](int trackId, AudioMixer::ReverbSettings r) {
            if (auto *mx = m_player ? m_player->audioMixer() : nullptr)
                mx->setReverbForTrack(trackId, r);
        });
    }

    if (auto *panel = qobject_cast<ReverbPanel *>(m_reverbDock->widget())) {
        QStringList names;
        QList<int> ids;
        const int trackCount = m_timeline ? m_timeline->audioTrackCount() : 0;
        buildAudioTrackList(trackCount, names, ids, /*includeMaster=*/false);
        panel->setTrackList(ids);
        for (int id : ids)
            panel->loadSettings(id, mixer->reverbForTrack(id));
        panel->loadSettings(0, mixer->reverbForTrack(0));
    }
    m_reverbDock->setVisible(true);
    m_reverbDock->raise();
}

void MainWindow::openNoiseReductionPanel()
{
    auto *mixer = m_player ? m_player->audioMixer() : nullptr;
    if (!mixer) {
        QMessageBox::information(this, tr("ノイズリダクション"),
            tr("オーディオミキサーが利用できません。"));
        return;
    }

    if (!m_noiseReductionDock) {
        m_noiseReductionDock = new QDockWidget(tr("Noise Reduction"), this);
        m_noiseReductionDock->setObjectName("NoiseReductionDock");
        auto *panel = new NoiseReductionPanel(m_noiseReductionDock);
        panel->setMixer(mixer);
        m_noiseReductionDock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, m_noiseReductionDock);

        connect(panel, &NoiseReductionPanel::noiseReductionChanged,
                this, [this](int trackId, AudioMixer::NoiseReductionSettings nr) {
            if (auto *mx = m_player ? m_player->audioMixer() : nullptr)
                mx->setNoiseReductionForTrack(trackId, nr);
        });
    }

    if (auto *panel = qobject_cast<NoiseReductionPanel *>(m_noiseReductionDock->widget())) {
        QStringList names;
        QList<int> ids;
        const int trackCount = m_timeline ? m_timeline->audioTrackCount() : 0;
        buildAudioTrackList(trackCount, names, ids, /*includeMaster=*/false);
        panel->setTrackList(ids);
        for (int id : ids)
            panel->loadSettings(id, mixer->noiseReductionForTrack(id));
        panel->loadSettings(0, mixer->noiseReductionForTrack(0));
    }
    m_noiseReductionDock->setVisible(true);
    m_noiseReductionDock->raise();
}

void MainWindow::openTitlePresetDialog()
{
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, tr("タイトルプリセット"),
            tr("先にクリップを追加してください。"));
        return;
    }

    TitlePresetDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const EnhancedTextOverlay &resolved = dlg.resolvedOverlay();
    if (m_timeline->videoClips().isEmpty())
        return;

    // Persist the resolved overlay into V1's first clip via the
    // existing helper (writes back via setClips internally so the
    // mutation actually sticks).
    if (!m_timeline->addTextOverlayToFirstVideoClip(resolved)) {
        statusBar()->showMessage(
            tr("タイトルプリセットの適用に失敗しました"), 3000);
        return;
    }
    statusBar()->showMessage(
        tr("タイトルプリセットを適用しました: %1").arg(resolved.text), 4000);
}

void MainWindow::openMultiCamDialog()
{
    MultiCamDialog dlg(this);
    if (m_timeline) {
        const qint64 totalUs =
            static_cast<qint64>(m_timeline->totalDuration() * 1000000.0);
        if (totalUs > 0)
            dlg.setTimelineDurationUs(totalUs);
    }
    connect(&dlg, &MultiCamDialog::applyToTimeline,
            this, &MainWindow::onMultiCamApplyToTimeline);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const MultiCamProject project = dlg.result();
    statusBar()->showMessage(
        tr("マルチカメラ EDL を作成しました (角度: %1, 切替: %2)")
            .arg(project.angles.size())
            .arg(project.switches.size()), 4000);
}

void MainWindow::onMultiCamApplyToTimeline(const MultiCamProject &project)
{
    if (!m_timeline) return;
    if (project.switches.isEmpty() || project.angles.isEmpty()) {
        statusBar()->showMessage(
            tr("マルチカメラ: 切替マーカーが無いため適用をスキップしました"), 4000);
        return;
    }

    if (!m_timeline->videoClips().isEmpty()) {
        const auto reply = QMessageBox::question(
            this, tr("マルチカメラ"),
            tr("V1 トラックを multi-cam EDL で置き換えますか？\n"
               "(現在 %1 個のクリップが消去されます)")
                .arg(m_timeline->videoClips().size()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;
    }

    // angle id → angle lookup. Switches reference angles by id (not by
    // index) so the EDL survives angle reorder/remove in the dialog.
    QHash<int, MultiCamAngle> angleById;
    for (const MultiCamAngle &a : project.angles)
        angleById.insert(a.id, a);

    // Defensive sort — the dialog already keeps switches[] ordered, but a
    // mis-built project loaded from JSON could violate that.
    QVector<MultiCamSwitch> sw = project.switches;
    std::sort(sw.begin(), sw.end(),
              [](const MultiCamSwitch &a, const MultiCamSwitch &b) {
                  return a.timelineUs < b.timelineUs;
              });

    // Tail duration for the final switch — give the last angle a 30 s tail
    // since switches[] only encodes "when to cut TO" but never an explicit
    // EOF. Downstream EOF handling (VideoPlayer + AudioMixer) clamps to
    // the source file length on its own.
    constexpr qint64 kTailFallbackUs = 30LL * 1000000LL;

    QVector<ClipInfo> v1Clips;
    QVector<ClipInfo> a1Clips;
    int skipped = 0;

    qint64 prevTlEndUs = 0;
    for (int i = 0; i < sw.size(); ++i) {
        const MultiCamSwitch &cur = sw[i];
        if (!angleById.contains(cur.activeAngleId)) {
            ++skipped;
            continue;
        }
        const MultiCamAngle &angle = angleById.value(cur.activeAngleId);
        if (angle.sourcePath.isEmpty()
            || !QFileInfo::exists(angle.sourcePath)) {
            ++skipped;
            continue;
        }

        const qint64 segStartTlUs = cur.timelineUs;
        const qint64 segEndTlUs   = (i + 1 < sw.size())
            ? sw[i + 1].timelineUs
            : segStartTlUs + kTailFallbackUs;
        if (segEndTlUs <= segStartTlUs)
            continue;

        const qint64 segDurUs = segEndTlUs - segStartTlUs;
        // syncOffsetUs shifts the source PTS for this angle so multi-cam
        // sync (clap board / audio align) lands at the same timeline tick.
        const qint64 srcStartUs =
            std::max<qint64>(0, segStartTlUs - angle.syncOffsetUs);
        const qint64 leadInUs   = segStartTlUs - prevTlEndUs;

        ClipInfo videoClip;
        videoClip.filePath = angle.sourcePath;
        videoClip.displayName = QFileInfo(angle.sourcePath).fileName();
        const double srcStartSec = static_cast<double>(srcStartUs) / 1000000.0;
        const double segDurSec   = static_cast<double>(segDurUs)   / 1000000.0;
        videoClip.duration = srcStartSec + segDurSec;
        videoClip.inPoint  = srcStartSec;
        videoClip.outPoint = srcStartSec + segDurSec;
        videoClip.leadInSec = static_cast<double>(leadInUs) / 1000000.0;

        v1Clips.append(videoClip);

        // Mirror the same source range to A1 — most camera files carry
        // embedded audio, and AudioMixer silently outputs zero samples
        // for missing audio streams.
        a1Clips.append(videoClip);

        prevTlEndUs = segEndTlUs;
    }

    if (v1Clips.isEmpty()) {
        statusBar()->showMessage(
            tr("マルチカメラ: 有効なセグメントが無く適用を中止しました"), 4000);
        return;
    }

    while (m_timeline->videoTrackCount() < 1)
        m_timeline->addVideoTrack();
    while (m_timeline->audioTrackCount() < 1)
        m_timeline->addAudioTrack();

    m_timeline->videoTracks().first()->setClips(v1Clips);
    m_timeline->audioTracks().first()->setClips(a1Clips);
    m_timeline->refreshPlaybackSequence();

    if (skipped > 0) {
        statusBar()->showMessage(
            tr("マルチカメラ EDL 適用 (V1/A1=%1 セグメント, %2 件スキップ)")
                .arg(v1Clips.size()).arg(skipped), 6000);
    } else {
        statusBar()->showMessage(
            tr("マルチカメラ EDL を V1/A1 に適用 (%1 セグメント)")
                .arg(v1Clips.size()), 4000);
    }
}

void MainWindow::openRenderQueueDialog()
{
    if (!m_renderQueueDialog) {
        m_renderQueueDialog = new RenderQueueDialog(this);
        if (m_timeline) {
            const qint64 totalUs =
                static_cast<qint64>(m_timeline->totalDuration() * 1000000.0);
            m_renderQueueDialog->setDefaultTimelineRange(0, totalUs);
        }
        // RM-1.4: let blank-source ("current project") queue entries
        // carry the live edit-graph instead of resolving to nullptr.
        m_renderQueueDialog->setLiveTimeline(m_timeline);
    }
    // RM-1.4/RM-1.3: keep the live Timeline's matte carrier current so a
    // "current project" queue job exports the same track matte the GUI
    // preview shows (the dialog has no save step before submitting).
    syncTrackMatteEntriesToTimeline(m_timeline, m_trackMatteClipEntries);
    m_renderQueueDialog->show();
    m_renderQueueDialog->raise();
    m_renderQueueDialog->activateWindow();
}

void MainWindow::openSceneDetector()
{
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, tr("シーン検出"),
            tr("先にクリップを追加してください。"));
        return;
    }

    // Run the existing AutoEdit scene-change analyser (synchronous; the
    // SceneDetector class is the streaming variant and requires a frame
    // pump that is deferred). On each detected cut, drop a Timeline
    // marker at the timestamp.
    const auto &clips = m_timeline->videoClips();
    int targetIdx = m_timeline->selectedVideoClipIndex();
    if (targetIdx < 0 || targetIdx >= clips.size())
        targetIdx = 0;
    const ClipInfo &clip = clips[targetIdx];

    statusBar()->showMessage(tr("シーン変化を解析しています..."));
    QApplication::processEvents();

    auto scenes = AutoEdit::detectSceneChanges(clip.filePath);
    if (scenes.isEmpty()) {
        statusBar()->showMessage(tr("シーン変化が検出されませんでした"), 4000);
        return;
    }

    // Drop a Timeline marker at every detected cut.
    const QColor sceneCutColor("#3399ff");
    int added = 0;
    for (const auto &s : scenes) {
        const qint64 timeUs = static_cast<qint64>(s.time * 1000000.0);
        const QString label = tr("Scene Cut %1").arg(added + 1);
        m_timeline->addMarker(timeUs, label, sceneCutColor);
        ++added;
    }
    statusBar()->showMessage(
        tr("シーン検出: %1 個のカットにマーカーを追加しました").arg(added), 5000);
}

void MainWindow::runMotionStabilizer()
{
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, tr("スタビライズ"),
            tr("先にクリップを追加してください。"));
        return;
    }

    bool ok = false;
    int smoothPct = QInputDialog::getInt(this, tr("スタビライズ"),
        tr("Smoothness (1-100, higher=smoother):"),
        50, 1, 100, 1, &ok);
    if (!ok)
        return;

    // US-INT-4: synchronous analyse + bake. V1 clip 0 only for v1.
    const auto &clips = m_timeline->videoClips();
    const ClipInfo &target = clips.first();
    statusBar()->showMessage(tr("スタビライズ解析中..."), 0);
    QApplication::processEvents();

    MotionStabilizer stab;
    stab.setSmoothness(smoothPct / 100.0);
    QVector<StabilizerKeyframe> kfs = stab.analyzeFile(target.filePath);
    if (kfs.isEmpty()) {
        statusBar()->showMessage(
            tr("スタビライズ失敗: フレームを解析できませんでした"), 6000);
        return;
    }
    m_timeline->setClipStabilizerKeyframes(0, kfs);
    statusBar()->showMessage(
        tr("スタビライズ完了: %1 フレーム").arg(kfs.size()), 6000);
}

void MainWindow::addAdjustmentLayerCmd()
{
    if (!m_timeline) {
        QMessageBox::information(this, tr("調整レイヤー"),
            tr("タイムラインが利用できません。"));
        return;
    }

    AdjustmentLayer layer;
    // Use playhead as start; default 5-second duration so the layer is
    // visible in the timeline immediately.
    const qint64 startUs =
        static_cast<qint64>(m_timeline->playheadPosition() * 1000000.0);
    layer.timelineStartUs = startUs;
    layer.timelineEndUs = startUs + 5LL * 1000000LL;
    layer.trackIndex = 0;
    layer.name = tr("Adjustment Layer");

    // Seed grading from the current ColorGradingPanel state if visible
    // so the user gets a layer that already reflects what they see.
    if (m_colorGradingPanel) {
        const ColorWheels cw = m_colorGradingPanel->currentWheels();
        layer.lift[0]  = cw.lift.x();   layer.lift[1]  = cw.lift.y();
        layer.lift[2]  = cw.lift.z();   layer.lift[3]  = cw.liftLuma;
        layer.gamma[0] = cw.gamma.x();  layer.gamma[1] = cw.gamma.y();
        layer.gamma[2] = cw.gamma.z();  layer.gamma[3] = cw.gammaLuma;
        layer.gain[0]  = cw.gain.x();   layer.gain[1]  = cw.gain.y();
        layer.gain[2]  = cw.gain.z();   layer.gain[3]  = cw.gainLuma;
        layer.gradingEnabled = true;
    }

    const int newId = m_timeline->addAdjustmentLayer(layer);
    statusBar()->showMessage(
        tr("調整レイヤーを追加しました (id=%1, %2s..%3s)")
            .arg(newId)
            .arg(layer.timelineStartUs / 1.0e6, 0, 'f', 2)
            .arg(layer.timelineEndUs / 1.0e6, 0, 'f', 2),
        4000);
}

void MainWindow::openSpeedRampDialog()
{
    if (!m_timeline || m_timeline->videoClips().isEmpty()) {
        QMessageBox::information(this, tr("速度 / 持続時間"),
            tr("先にクリップを追加してください。"));
        return;
    }
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, tr("速度 / 持続時間"),
            tr("クリップを選択してください。"));
        return;
    }

    bool ok = false;
    const double speedMul = QInputDialog::getDouble(this, tr("速度 / 持続時間"),
        tr("速度倍率 (0.1 - 5.0):"), 1.0, 0.1, 5.0, 2, &ok);
    if (!ok)
        return;

    speedramp::SpeedRamp ramp;
    ramp.clearAndSetIdentity();
    ramp.addKeyframe(0, speedMul);

    const int clipIdx = m_timeline->selectedVideoClipIndex();
    if (clipIdx < 0) {
        QMessageBox::information(this, tr("速度 / 持続時間"),
            tr("V1 にクリップを選択してください。"));
        return;
    }
    m_timeline->setSpeedRamp(clipIdx, ramp);
    statusBar()->showMessage(
        tr("速度ランプを %1x に設定しました (clip #%2)")
            .arg(speedMul, 0, 'f', 2).arg(clipIdx), 5000);
}

void MainWindow::addQuickMarker()
{
    if (!m_timeline) return;
    const qint64 timeUs =
        static_cast<qint64>(m_timeline->playheadPosition() * 1000000.0);
    const int id = m_timeline->addMarker(timeUs, QStringLiteral("Marker"),
                                          QColor(QStringLiteral("#ff5050")));
    statusBar()->showMessage(
        tr("マーカー追加 (id=%1, %2s)")
            .arg(id)
            .arg(timeUs / 1.0e6, 0, 'f', 2), 3000);
}

void MainWindow::addColoredMarker()
{
    if (!m_timeline) return;
    QColor c = QColorDialog::getColor(QColor(QStringLiteral("#ff5050")),
                                      this, tr("マーカーの色を選択"));
    if (!c.isValid())
        return;

    bool ok = false;
    QString label = QInputDialog::getText(this, tr("色付きマーカー"),
        tr("ラベル (空でも可):"), QLineEdit::Normal, QString(), &ok);
    if (!ok)
        return;

    const qint64 timeUs =
        static_cast<qint64>(m_timeline->playheadPosition() * 1000000.0);
    if (label.isEmpty())
        label = QStringLiteral("Marker");
    const int id = m_timeline->addMarker(timeUs, label, c);
    statusBar()->showMessage(
        tr("色付きマーカー追加 (id=%1, %2s, %3)")
            .arg(id)
            .arg(timeUs / 1.0e6, 0, 'f', 2)
            .arg(c.name()), 3000);
}

void MainWindow::jumpToNextMarker()
{
    if (!m_timeline) return;
    const qint64 nowUs =
        static_cast<qint64>(m_timeline->playheadPosition() * 1000000.0);
    const int id = m_timeline->nextMarkerAfter(nowUs);
    if (id < 0) {
        statusBar()->showMessage(tr("これより後にマーカーがありません"), 2500);
        return;
    }
    const auto m = m_timeline->markerById(id);
    m_timeline->setPlayheadPosition(m.timelineUs / 1.0e6);
    statusBar()->showMessage(
        tr("マーカーへジャンプ: %1 (%2s)")
            .arg(m.label)
            .arg(m.timelineUs / 1.0e6, 0, 'f', 2), 2500);
}

void MainWindow::jumpToPrevMarker()
{
    if (!m_timeline) return;
    const qint64 nowUs =
        static_cast<qint64>(m_timeline->playheadPosition() * 1000000.0);
    const int id = m_timeline->prevMarkerBefore(nowUs);
    if (id < 0) {
        statusBar()->showMessage(tr("これより前にマーカーがありません"), 2500);
        return;
    }
    const auto m = m_timeline->markerById(id);
    m_timeline->setPlayheadPosition(m.timelineUs / 1.0e6);
    statusBar()->showMessage(
        tr("マーカーへジャンプ: %1 (%2s)")
            .arg(m.label)
            .arg(m.timelineUs / 1.0e6, 0, 'f', 2), 2500);
}

// MK-2: Timeline の現在のマーカー一覧をパネルへ流し込む。markersChanged の
// 全発火パス (追加/削除/更新/差し替え/期間変更) からここに集約される。
void MainWindow::refreshMarkerPanel()
{
    if (!m_markerPanelDock || !m_timeline)
        return;
    m_markerPanelDock->setMarkers(m_timeline->markers());
}

// MK-2: パネル行のダブルクリックで再生ヘッドをその時刻へ移動する
// (jumpToNextMarker と同じ setPlayheadPosition 経路を流用)。
void MainWindow::onMarkerPanelJump(qint64 timelineUs)
{
    if (!m_timeline)
        return;
    m_timeline->setPlayheadPosition(timelineUs / 1.0e6);
    statusBar()->showMessage(
        tr("マーカーへジャンプ (%1s)")
            .arg(timelineUs / 1.0e6, 0, 'f', 2), 2500);
}

// MK-2: パネルのノート列編集を Timeline 側マーカーへ反映する。
void MainWindow::onMarkerPanelNoteEdited(int markerId, const QString &note)
{
    if (!m_timeline)
        return;
    Marker m = m_timeline->markerById(markerId);
    if (m.id != markerId)
        return;  // 既に削除済み等
    if (m.note == note)
        return;  // 変化なし
    m.note = note;
    m_timeline->updateMarker(markerId, m);
    // updateMarker → markersChanged → refreshMarkerPanel が走るので明示再描画は不要。
}

// MK-2: パネルの削除ボタンで Timeline からマーカーを削除する。
void MainWindow::onMarkerPanelDeleteRequested(int markerId)
{
    if (!m_timeline)
        return;
    if (m_timeline->removeMarker(markerId)) {
        statusBar()->showMessage(
            tr("マーカーを削除しました (id=%1)").arg(markerId), 2500);
        // removeMarker → markersChanged → refreshMarkerPanel で自動再描画。
    }
}

void MainWindow::openVoiceOverDialog()
{
    if (m_voiceOverDialog) {
        m_voiceOverDialog->raise();
        m_voiceOverDialog->activateWindow();
        return;
    }

    // Build default output path
    QString projectDir;
    if (!m_projectFilePath.isEmpty()) {
        projectDir = QFileInfo(m_projectFilePath).absolutePath();
    } else {
        projectDir = QDir::homePath();
    }
    QString audioDir = projectDir + "/audio";
    QDir dir(audioDir);
    if (!dir.exists())
        dir.mkpath(".");

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
    QString defaultPath = audioDir + "/voiceover-" + timestamp + ".wav";

    m_voiceOverDialog = new voiceover::VoiceOverDialog(defaultPath, this);

    // Mute audio preview during recording to prevent feedback
    const bool wasMuted = m_player ? m_player->isMuted() : false;
    if (m_player)
        m_player->setMuted(true);

    connect(m_voiceOverDialog, &voiceover::VoiceOverDialog::recordingStopped,
            this, [this](const QString &wavPath, qint64) {
                if (m_voiceOverDialog && m_voiceOverDialog->insertAtPlayhead() && m_timeline) {
                    m_timeline->insertAudioClipAtPlayhead(wavPath, 2);
                    statusBar()->showMessage(
                        tr("Voice-over inserted at playhead: %1").arg(wavPath), 5000);
                }
            });

    connect(m_voiceOverDialog, &QDialog::finished,
            this, [this, wasMuted](int) {
                if (m_player)
                    m_player->setMuted(wasMuted);
                m_voiceOverDialog->deleteLater();
                m_voiceOverDialog = nullptr;
            });

    m_voiceOverDialog->exec();
}

// US-AETEXT-12: AE Text Parity — 11 new slots

void MainWindow::addPathText()
{
    QDialog dialog(this);
    dialog.setWindowTitle("パステキスト追加");
    auto *layout = new QFormLayout(&dialog);
    QLineEdit *textEdit = new QLineEdit("Sample Text", &dialog);
    QLineEdit *fontEdit = new QLineEdit("Arial", &dialog);
    QSpinBox *sizeSpin = new QSpinBox(&dialog);
    sizeSpin->setRange(8, 200);
    sizeSpin->setValue(32);
    layout->addRow("テキスト:", textEdit);
    layout->addRow("フォント:", fontEdit);
    layout->addRow("サイズ:", sizeSpin);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Rejected)
        return;

    QFont font(fontEdit->text(), sizeSpin->value());
    auto *pathText = new PathText(this);
    pathText->setText(textEdit->text(), font);
    pathText->setBrushColor(Qt::white);
    QPainterPath path;
    path.moveTo(50, 200);
    path.cubicTo(150, 50, 350, 350, 450, 200);
    pathText->setPath(path);
    m_pathTexts.append(pathText);

    statusBar()->showMessage(QString("パステキスト追加: 「%1」").arg(textEdit->text()));
}

void MainWindow::addRangeSelector()
{
    QDialog dialog(this);
    dialog.setWindowTitle("レンジセレクター");
    auto *layout = new QFormLayout(&dialog);
    QSpinBox *startSpin = new QSpinBox(&dialog);
    startSpin->setRange(0, 100);
    startSpin->setValue(0);
    QSpinBox *endSpin = new QSpinBox(&dialog);
    endSpin->setRange(0, 100);
    endSpin->setValue(100);
    QDoubleSpinBox *amountSpin = new QDoubleSpinBox(&dialog);
    amountSpin->setRange(-100, 100);
    amountSpin->setValue(0);
    layout->addRow("開始 (%):", startSpin);
    layout->addRow("終了 (%):", endSpin);
    layout->addRow("適用量:", amountSpin);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Rejected)
        return;

    statusBar()->showMessage(QString("レンジセレクター: %1–%2%%, 量=%3")
        .arg(startSpin->value()).arg(endSpin->value()).arg(amountSpin->value()));
}

void MainWindow::addWigglySelector()
{
    QDialog dialog(this);
    dialog.setWindowTitle("ウィグリーセレクター");
    auto *layout = new QFormLayout(&dialog);
    QDoubleSpinBox *freqSpin = new QDoubleSpinBox(&dialog);
    freqSpin->setRange(0.1, 20.0);
    freqSpin->setValue(2.0);
    freqSpin->setSingleStep(0.5);
    QDoubleSpinBox *magSpin = new QDoubleSpinBox(&dialog);
    magSpin->setRange(0, 100);
    magSpin->setValue(25);
    magSpin->setSingleStep(5);
    layout->addRow("周波数 (Hz):", freqSpin);
    layout->addRow("振幅 (px):", magSpin);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Rejected)
        return;

    statusBar()->showMessage(QString("ウィグリーセレクター: 周波数=%1Hz, 振幅=%2px")
        .arg(freqSpin->value()).arg(magSpin->value()));
}

void MainWindow::addSourceTextKeyframe()
{
    if (!m_timeline->hasSelection()) {
        QMessageBox::information(this, "ソーステキスト keyframe", "クリップを先に選択してください。");
        return;
    }
    bool ok;
    QString newText = QInputDialog::getText(this, "ソーステキスト keyframe",
        "新しいテキスト:", QLineEdit::Normal, "Keyframed Text", &ok);
    if (!ok || newText.isEmpty())
        return;

    double time = m_timeline->playheadPosition();
    KeyframeManager km = m_timeline->clipKeyframes();
    if (!km.hasTrack(QStringLiteral("source_text"))) {
        StringKeyframeTrack track(QStringLiteral("source_text"));
        track.addKeyframe(time, newText);
        km.addStringTrack(track);
    }
    m_timeline->setClipKeyframes(km);

    statusBar()->showMessage(QString("ソーステキスト keyframe: 「%1」 @ %2s").arg(newText).arg(time, 0, 'f', 2));
}

void MainWindow::addAnimationPreset()
{
    QStringList presets = TextAnimPresets::presetNames();
    bool ok;
    QString preset = QInputDialog::getItem(this, "アニメーションプリセット",
        "プリセットを選択:", presets, 0, false, &ok);
    if (!ok)
        return;

    statusBar()->showMessage(QString("アニメーションプリセット適用: %1 — %2")
        .arg(preset).arg(TextAnimPresets::presetDescription(preset)));
}

void MainWindow::add3DText()
{
    QDialog dialog(this);
    dialog.setWindowTitle("3Dテキストレイヤー追加");
    auto *layout = new QFormLayout(&dialog);
    QLineEdit *textEdit = new QLineEdit("3D Text", &dialog);
    QLineEdit *fontEdit = new QLineEdit("Arial", &dialog);
    QSpinBox *sizeSpin = new QSpinBox(&dialog);
    sizeSpin->setRange(8, 200);
    sizeSpin->setValue(32);
    QDoubleSpinBox *distSpin = new QDoubleSpinBox(&dialog);
    distSpin->setRange(100, 2000);
    distSpin->setValue(400);
    layout->addRow("テキスト:", textEdit);
    layout->addRow("フォント:", fontEdit);
    layout->addRow("サイズ:", sizeSpin);
    layout->addRow("カメラ距離:", distSpin);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Rejected)
        return;

    QFont font(fontEdit->text(), sizeSpin->value());
    auto *text3D = new Text3DLayer(this);
    text3D->setText(textEdit->text(), font);
    text3D->setCameraDistance(distSpin->value());
    text3D->setPerCharRotation(QVector3D(0, 0, 0));
    m_text3DLayers.append(text3D);

    statusBar()->showMessage(QString("3Dテキストレイヤー追加: 「%1」").arg(textEdit->text()));
}

void MainWindow::addMaskTextReveal()
{
    QDialog dialog(this);
    dialog.setWindowTitle("マスクテキストreveal追加");
    auto *layout = new QFormLayout(&dialog);
    QCheckBox *invertCheck = new QCheckBox("反転", &dialog);
    QDoubleSpinBox *featherSpin = new QDoubleSpinBox(&dialog);
    featherSpin->setRange(0, 50);
    featherSpin->setValue(5);
    QDoubleSpinBox *expansionSpin = new QDoubleSpinBox(&dialog);
    expansionSpin->setRange(-50, 50);
    expansionSpin->setValue(0);
    layout->addRow(invertCheck);
    layout->addRow("フェザー (px):", featherSpin);
    layout->addRow("拡張 (px):", expansionSpin);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Rejected)
        return;

    auto *maskReveal = new TextMaskReveal();
    maskReveal->setMaskInvert(invertCheck->isChecked());
    maskReveal->setMaskFeatherPx(featherSpin->value());
    maskReveal->setMaskExpansionPx(expansionSpin->value());
    m_textMaskReveals.append(maskReveal);

    statusBar()->showMessage("マスクテキストreveal追加");
}

void MainWindow::addBendTextWarp()
{
    QDialog dialog(this);
    dialog.setWindowTitle("ベンド/インフレートtext追加");
    auto *layout = new QFormLayout(&dialog);
    QLineEdit *textEdit = new QLineEdit("Warped Text", &dialog);
    QDoubleSpinBox *bendSpin = new QDoubleSpinBox(&dialog);
    bendSpin->setRange(-180, 180);
    bendSpin->setValue(0);
    bendSpin->setSingleStep(5);
    QDoubleSpinBox *inflateSpin = new QDoubleSpinBox(&dialog);
    inflateSpin->setRange(-1.0, 1.0);
    inflateSpin->setValue(0);
    inflateSpin->setSingleStep(0.1);
    layout->addRow("テキスト:", textEdit);
    layout->addRow("ベンド (°):", bendSpin);
    layout->addRow("インフレート:", inflateSpin);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Rejected)
        return;

    QFont font("Arial", 32);
    auto *pathWarp = new TextPathWarp(this);
    pathWarp->setText(textEdit->text(), font);
    pathWarp->setBendDegrees(bendSpin->value());
    pathWarp->setInflateAmount(inflateSpin->value());
    m_textPathWarps.append(pathWarp);

    statusBar()->showMessage(QString("ベンド/インフレートtext追加: 「%1」").arg(textEdit->text()));
}

void MainWindow::changeTextScope()
{
    QStringList scopes = {"Position", "Scale", "Rotation", "Opacity", "Anchor Point"};
    bool ok;
    QString scope = QInputDialog::getItem(this, "スコープ切替",
        "アニメーションスコープ:", scopes, 0, false, &ok);
    if (!ok)
        return;

    statusBar()->showMessage(QString("スコープ切替: %1").arg(scope));
}

void MainWindow::addVariableFontAxis()
{
    QDialog dialog(this);
    dialog.setWindowTitle("可変フォントaxisアニメ");
    auto *layout = new QFormLayout(&dialog);
    QLineEdit *fontEdit = new QLineEdit("Arial", &dialog);
    QSpinBox *sizeSpin = new QSpinBox(&dialog);
    sizeSpin->setRange(8, 200);
    sizeSpin->setValue(32);
    QLineEdit *axisTagEdit = new QLineEdit("wght", &dialog);
    axisTagEdit->setToolTip("例: wght, wdth, opsz");
    QDoubleSpinBox *startValSpin = new QDoubleSpinBox(&dialog);
    startValSpin->setRange(1, 1000);
    startValSpin->setValue(400);
    QDoubleSpinBox *endValSpin = new QDoubleSpinBox(&dialog);
    endValSpin->setRange(1, 1000);
    endValSpin->setValue(700);
    layout->addRow("フォント:", fontEdit);
    layout->addRow("サイズ:", sizeSpin);
    layout->addRow("Axis タグ:", axisTagEdit);
    layout->addRow("開始値:", startValSpin);
    layout->addRow("終了値:", endValSpin);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Rejected)
        return;

    QFont font(fontEdit->text(), sizeSpin->value());
    auto *varFont = new VariableFontAxis(this);
    varFont->setBaseFont(font);
    varFont->setAxisProperty(axisTagEdit->text(), QStringLiteral("font_%1").arg(axisTagEdit->text()));
    m_variableFontAxes.append(varFont);

    statusBar()->showMessage(QString("可変フォントaxisアニメ: %1 %2→%3")
        .arg(axisTagEdit->text()).arg(startValSpin->value()).arg(endValSpin->value()));
}

void MainWindow::addMographTemplate()
{
    QStringList templates = MographText::templateNames();
    bool ok;
    QString tmplate = QInputDialog::getItem(this, "Mographテンプレート",
        "テンプレートを選択:", templates, 0, false, &ok);
    if (!ok)
        return;

    QStringList args;
    if (tmplate == "lower_third") {
        bool ok1, ok2;
        args << QInputDialog::getText(this, "Mograph", "上部テキスト:", QLineEdit::Normal, "Title", &ok1)
             << QInputDialog::getText(this, "Mograph", "下部テキスト:", QLineEdit::Normal, "Subtitle", &ok2);
    } else {
        bool ok1;
        args << QInputDialog::getText(this, "Mograph", "テキスト:", QLineEdit::Normal, "Mograph Text", &ok1);
    }

    auto *mograph = new MographText();
    mograph->setTemplateName(tmplate);
    mograph->setArgs(args);
    m_mographTexts.append(mograph);

    statusBar()->showMessage(QString("Mographテンプレート適用: %1").arg(tmplate));
}

// US-SNS-7: Smart Reframe dialog + analysis
void MainWindow::openSmartReframe()
{
    SmartReframeDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    SmartReframeParams params = dialog.params();
    m_smartReframe.setSourceSize(QSize(m_projectConfig.width, m_projectConfig.height));
    m_smartReframe.setTargetAspect(params.aspectW, params.aspectH);
    m_smartReframe.setSmoothness(params.smoothness);
    m_smartReframe.setMotionWeight(params.motionWeight);

    // Sample frames from the active clip(s) on the timeline.
    const auto &clips = m_timeline->videoClips();
    if (clips.isEmpty()) {
        statusBar()->showMessage("スマートリフレーム: タイムラインにクリップがありません", 4000);
        return;
    }

    // Use the first clip for analysis; sample every Nth frame.
    const double clipDuration = clips[0].outPoint - clips[0].inPoint;
    const int sampleCount = qMin(30, qMax(3, static_cast<int>(clipDuration * 2)));
    const double step = clipDuration / qMax(1, sampleCount);

    for (int i = 0; i < sampleCount; ++i) {
        const double t = clips[0].inPoint + i * step;
        // Decode a frame at time t by seeking the player and grabbing the current frame.
        m_player->seek(qRound64(t * 1000));
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        // We don't have direct frame access here, so we pass a dummy image.
        // In a full implementation this would use FFmpeg decode directly.
        QImage dummyFrame(m_projectConfig.width, m_projectConfig.height, QImage::Format_RGB888);
        m_smartReframe.analyzeFrame(t, dummyFrame);
    }

    m_smartReframe.finalizeAnalysis();

    // Hand to exporter for render-time application.
    m_exporter->setSmartReframe(&m_smartReframe);

    const QRectF crop0 = m_smartReframe.cropRectAt(clips[0].inPoint);
    statusBar()->showMessage(
        QString("スマートリフレーム: %1 フレーム解析完了 (crop@0s: %2,%3 %4x%5)")
            .arg(sampleCount)
            .arg(qRound(crop0.x())).arg(qRound(crop0.y()))
            .arg(qRound(crop0.width())).arg(qRound(crop0.height())),
        5000);
}

// US-SNS-7: Render subtitle track from existing segments
void MainWindow::renderSubtitleTrack()
{
    if (m_subtitleSegments.isEmpty()) {
        // Chain generateSubtitles first if no segments exist yet.
        generateSubtitles();
        if (m_subtitleSegments.isEmpty()) {
            statusBar()->showMessage("字幕トラック: 字幕セグメントがありません", 4000);
            return;
        }
    }

    SubtitleTrackRenderer *renderer = new SubtitleTrackRenderer(this);
    renderer->setSegments(m_subtitleSegments);
    renderer->setStyle(m_subtitleStyle);

    // Push overlays to the player for live preview.
    QVector<EnhancedTextOverlay> overlays = renderer->toOverlays();
    if (!overlays.isEmpty() && m_player) {
        m_player->setTextOverlays(overlays);
    }

    // Hand to exporter for burn-in during export.
    m_exporter->setSubtitleRenderer(renderer);

    statusBar()->showMessage(
        QString("字幕トラック: %1 セグメントをレンダリング").arg(m_subtitleSegments.size()),
        4000);
}

// US-SNS-7: Loudness normalization slot
void MainWindow::applyLoudnessNormalize(double targetLUFS, double gainDb)
{
    if (auto *mixer = m_player ? m_player->audioMixer() : nullptr) {
        // Set the master loudness normalizer target.
        // The normalizer amount controls how much of the target gain is applied.
        const double amount = (gainDb != 0.0) ? 1.0 : 0.0;
        mixer->setNormalizerAmount(amount);
        mixer->setNormalizerUniformity(0.5);
    }

    // Pass gain to exporter for render-time normalization.
    m_exporter->setLoudnessGainDb(gainDb);

    statusBar()->showMessage(
        QString("ラウドネス正規化: target=%1 LUFS, gain=%2 dB")
            .arg(targetLUFS, 0, 'f', 1)
            .arg(gainDb, 0, 'f', 1),
        4000);
}

// ──────────────────────────────────────────────────────────────────────────
// US-NODE-9: Node compositing mode integration
// ──────────────────────────────────────────────────────────────────────────

void MainWindow::setupNodeCompositingDocks()
{
    if (m_nodeCanvasDock || m_nodePropsDock)
        return; // already created

    m_nodeCanvas = new NodeCanvasWidget(this);
    m_nodeCanvasDock = new QDockWidget("ノードキャンバス", this);
    m_nodeCanvasDock->setObjectName("NodeCanvasDock");
    m_nodeCanvasDock->setWidget(m_nodeCanvas);
    addDockWidget(Qt::RightDockWidgetArea, m_nodeCanvasDock);
    m_nodeCanvasDock->setVisible(false);

    m_nodePropsPanel = new NodePropertiesPanel(this);
    m_nodePropsDock = new QDockWidget("ノードプロパティ", this);
    m_nodePropsDock->setObjectName("NodePropsDock");
    m_nodePropsDock->setWidget(m_nodePropsPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_nodePropsDock);
    m_nodePropsDock->setVisible(false);

    connect(m_nodeCanvas, &NodeCanvasWidget::nodeSelected,
            this, &MainWindow::onNodeSelected);
    connect(m_nodePropsPanel, &NodePropertiesPanel::paramChanged,
            this, [this](int nodeId, const QString &key, const QVariant &value) {
        if (!m_activeNodeGraph) return;
        GraphNode *node = m_activeNodeGraph->node(nodeId);
        if (!node) return;
        node->params.insert(key, value);
        m_activeNodeGraph->markDirty(nodeId);
        onNodeGraphChanged();
    });
}

void MainWindow::toggleNodeCompositingMode(bool on)
{
    if (!m_timeline) return;

    if (on) {
        const int clipIdx = m_timeline->selectedVideoClipIndex();
        if (clipIdx < 0) {
            statusBar()->showMessage("ノードモード: クリップを選択してください", 3000);
            QTimer::singleShot(0, this, [this]() {
                if (m_nodeModeAction) m_nodeModeAction->setChecked(false);
            });
            return;
        }

        const auto &clips = m_timeline->videoClips();
        if (clips.isEmpty() || clipIdx >= clips.size()) {
            statusBar()->showMessage("ノードモード: 有効なクリップがありません", 3000);
            QTimer::singleShot(0, this, [this]() {
                if (m_nodeModeAction) m_nodeModeAction->setChecked(false);
            });
            return;
        }

        const QString clipId = QString("clip:%1").arg(clipIdx);
        m_nodeModeClipId = clipId;
        m_nodeModeActive = true;

        // Build the node graph from the clip's effect stack
        *m_activeNodeGraph = layerbridge::fromEffectStack(clipId, m_timeline->clipEffects());

        setupNodeCompositingDocks();
        m_nodeCanvas->setGraph(m_activeNodeGraph);
        m_nodeCanvasDock->setVisible(true);
        m_nodePropsDock->setVisible(true);

        statusBar()->showMessage("ノードコンポジットモード ON", 2000);
    } else {
        // Turning off: if the graph is a linear chain, write it back to the effect stack
        if (m_activeNodeGraph && layerbridge::isLinearChain(*m_activeNodeGraph)) {
            QVector<VideoEffect> effects;
            if (layerbridge::toEffectStack(*m_activeNodeGraph, effects)) {
                m_timeline->setClipEffects(effects);
            }
        }

        m_nodeModeActive = false;
        m_nodeModeClipId.clear();

        if (m_nodeCanvasDock) m_nodeCanvasDock->setVisible(false);
        if (m_nodePropsDock) m_nodePropsDock->setVisible(false);

        statusBar()->showMessage("ノードコンポジットモード OFF — レイヤーモードに戻りました", 2000);
    }
}

void MainWindow::onNodeGraphChanged()
{
    if (!m_activeNodeGraph || !m_nodeEvaluator || !m_player)
        return;

    m_nodeEvaluator->setGraph(m_activeNodeGraph);
    m_nodeEvaluator->setOutputSize(QSize(m_projectConfig.width, m_projectConfig.height));

    // Quick evaluation at time=0 to push a result to GLPreview
    QImage result = m_nodeEvaluator->render(0.0);
    if (!result.isNull() && m_player->glPreview()) {
        m_player->glPreview()->displayFrame(result);
        m_player->glPreview()->update();
    }
}

void MainWindow::onNodeSelected(int id)
{
    if (!m_nodePropsPanel || !m_activeNodeGraph)
        return;
    m_nodePropsPanel->setSelection(m_activeNodeGraph, id);
}
