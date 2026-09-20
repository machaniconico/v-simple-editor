#include "../ExportDialog.h"
#include "../RenderQueue.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../libavcore/Encode.h"
#include "../libavcore/FrameGrab.h"
#include <QEventLoop>
#include <QFile>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <cstdio>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace {
using Mode = subtitle::ExportMode;

bool sourceClip(const QString &path)
{
    libavcore::EncodeRequest request;
    request.outputPath = path.toUtf8().toStdString();
    request.width = 320; request.height = 180; request.fps = 12;
    request.videoCodecName = "mpeg4";
    request.videoBitrateBits = 1500000;
    request.audioEncode = true;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage image(320, 180, QImage::Format_RGB888);
    image.fill(QColor(30, 50, 90));
    for (int i = 0; i < 36; ++i)
        if (!encoder.pushFrameRgb24(image.constBits(), image.bytesPerLine(), i)) return false;
    AVFrame *audio = av_frame_alloc();
    if (!audio) return false;
    audio->format = AV_SAMPLE_FMT_FLTP;
    audio->sample_rate = 48000;
    audio->nb_samples = 1024;
    av_channel_layout_default(&audio->ch_layout, 2);
    bool ok = av_frame_get_buffer(audio, 0) >= 0;
    for (int i = 0; ok && i < 144000; i += 1024) {
        ok = av_frame_make_writable(audio) >= 0;
        if (ok) {
            av_samples_set_silence(audio->extended_data, 0, 1024, 2, AV_SAMPLE_FMT_FLTP);
            ok = encoder.pushAudioFrame(audio, i);
        }
    }
    av_frame_free(&audio);
    return ok && !encoder.finalize();
}

struct Streams {
    int total = 0, video = 0, audio = 0, subtitle = 0;
    bool movText = true;
    std::vector<libavcore::SubtitleCue> cues;
};
bool inspect(const QString &path, Streams &out)
{
    AVFormatContext *format = nullptr;
    if (avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) < 0) return false;
    bool ok = avformat_find_stream_info(format, nullptr) >= 0;
    out.total = static_cast<int>(format->nb_streams);
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const auto *par = format->streams[i]->codecpar;
        out.video += par->codec_type == AVMEDIA_TYPE_VIDEO;
        out.audio += par->codec_type == AVMEDIA_TYPE_AUDIO;
        if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            ++out.subtitle;
            out.movText &= par->codec_id == AV_CODEC_ID_MOV_TEXT;
        }
    }
    AVPacket *packet = av_packet_alloc();
    if (!packet) ok = false;
    while (packet && av_read_frame(format, packet) >= 0) {
        const auto *stream = format->streams[packet->stream_index];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && packet->size >= 2) {
            const int size = (packet->data[0] << 8) | packet->data[1];
            if (size > packet->size - 2) ok = false;
            else if (size > 0) { // MOV also inserts empty samples to clear gaps.
                const int64_t start = av_rescale_q(packet->pts, stream->time_base, AVRational{1,1000000});
                const int64_t end = av_rescale_q(packet->pts + packet->duration, stream->time_base, AVRational{1,1000000});
                out.cues.push_back({start, end, std::string(reinterpret_cast<const char *>(packet->data + 2), size)});
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&format);
    return ok;
}
bool matches(const Streams &streams, const std::vector<libavcore::SubtitleCue> &expected)
{
    if (streams.total != 3 || streams.video != 1 || streams.audio != 1
        || streams.subtitle != 1 || !streams.movText || streams.cues.size() != expected.size()) return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (std::abs(streams.cues[i].startUs - expected[i].startUs) > 1000
            || std::abs(streams.cues[i].endUs - expected[i].endUs) > 1000
            || streams.cues[i].text != expected[i].text) return false;
    }
    return true;
}

bool render(Timeline &timeline, const QString &path, const QString &audio,
            const QJsonObject &config, qint64 startUs = 0, qint64 endUs = 3000000)
{
    RenderJob job;
    job.timeline = &timeline;
    job.outputPath = path;
    job.projectFilePath = audio;
    job.codec = "mpeg4";
    job.width = 320; job.height = 180;
    job.startUs = startUs; job.endUs = endUs;
    job.exportConfig = config;
    job.exportConfig["fps"] = 12;
    RenderQueue queue;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool done = false, success = false;
    QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
        [&](const QString &, bool ok, const QString &error) {
            done = true; success = ok;
            if (!ok) std::fprintf(stderr, "sub-movtext: %s\n", qPrintable(error));
            loop.quit();
        });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    queue.addJob(job);
    timer.start(60000);
    queue.start();
    if (!done) loop.exec();
    if (!done) queue.stop();
    return done && success;
}
}

int runSubMovtextSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    QTemporaryDir temp;
    const QString source = temp.filePath("source.mp4");
    bool ready = temp.isValid() && sourceClip(source);
    Timeline timeline;
    timeline.setProjectOutputConfig(320, 180, true);
    ClipInfo clip;
    clip.filePath = source; clip.duration = 3; clip.outPoint = 3;
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}}, QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    timeline.refreshPlaybackSequence();
    QVector<SubtitleSegment> segments;
    SubtitleSegment first; first.startTime = 0.25; first.endTime = 1.0; first.text = QStringLiteral("字幕 A / hello");
    SubtitleSegment second; second.startTime = 1.5; second.endTime = 2.5; second.text = QStringLiteral("字幕 B\nsecond line");
    segments << first << second;
    QObject owner;
    SubtitleTrackRenderer installed(&owner);
    SubtitleStyle style; style.font.setPointSize(16);
    installed.setSegments(segments); installed.setStyle(style);
    auto settings = [&](Mode mode) {
        ExportConfig config; config.subtitleMode = mode;
        QJsonObject json;
        config.appendSubtitleSettings(json);
        RenderQueue::appendSubtitleTrack(json, segments, style, &owner);
        return json;
    };
    const auto embedded = settings(Mode::MovText);
    const std::vector<libavcore::SubtitleCue> expected{
        {250000, 1000000, first.text.toUtf8().toStdString()},
        {1500000, 2500000, second.text.toUtf8().toStdString()}};
    bool g1 = ready;
    for (const QString &ext : {QStringLiteral("mp4"), QStringLiteral("mov")}) {
        Streams streams;
        const QString path = temp.filePath("embedded." + ext);
        g1 &= ready && render(timeline, path, source, embedded) && inspect(path, streams) && matches(streams, expected);
    }
    Streams trimmed;
    g1 &= ready && render(timeline, temp.filePath("trim.mp4"), source, embedded, 500000, 2000000)
        && inspect(temp.filePath("trim.mp4"), trimmed)
        && matches(trimmed, {{0, 500000, first.text.toUtf8().toStdString()},
                            {1000000, 1500000, second.text.toUtf8().toStdString()}});
    gate(1, g1);

    // Existing branch stays in production. Disable ONLY the additive mux
    // branch; compare outputs with it enabled, and require zero invocations.
    bool g2 = ready && ExportConfig{}.subtitleMode == Mode::BurnIn;
    for (const QString &route : {QStringLiteral("normal"), QStringLiteral("batch")}) {
        QJsonObject config = settings(Mode::BurnIn);
        const QString baseline = temp.filePath(route + "-baseline.mp4");
        const QString output = temp.filePath(route + "-default.mp4");
        libavcore::FrameEncoder::resetSubtitleStreamCountForTest();
        libavcore::FrameEncoder::setSubtitleMuxEnabledForTest(false);
        const bool baseOk = ready && render(timeline, baseline, source, config);
        libavcore::FrameEncoder::setSubtitleMuxEnabledForTest(true);
        Streams streams;
        const bool outputOk = ready && render(timeline, output, source, config) && inspect(output, streams);
        const QImage a = baseOk ? libavcore::grabFrameAt(baseline, 0.5) : QImage();
        const QImage b = outputOk ? libavcore::grabFrameAt(output, 0.5) : QImage();
        g2 &= baseOk && outputOk && !a.isNull() && a == b && streams.total == 2
            && streams.video == 1 && streams.audio == 1 && streams.subtitle == 0
            && libavcore::FrameEncoder::subtitleStreamCountForTest() == 0;
    }
    gate(2, g2);

    ExportDialog dialog(ProjectConfig{});
    auto *combo = dialog.findChild<QComboBox *>("exportSubtitleMode");
    bool g3 = combo && combo->count() == 4 && combo->currentData().toInt() == 1;
    QComboBox *codec = nullptr;
    for (auto *candidate : dialog.findChildren<QComboBox *>())
        if (candidate->findData("libvpx-vp9") >= 0 && candidate->findData("libx264") >= 0) codec = candidate;
    g3 &= codec != nullptr;
    if (codec && combo) {
        codec->setCurrentIndex(codec->findData("libvpx-vp9"));
        auto *model = qobject_cast<QStandardItemModel *>(combo->model());
        g3 &= model && !model->item(2)->isEnabled() && !model->item(3)->isEnabled()
            && !model->item(2)->toolTip().isEmpty();
        codec->setCurrentIndex(codec->findData("libx264"));
        g3 &= model && model->item(2)->isEnabled() && model->item(3)->isEnabled();
    }
    libavcore::EncodeRequest invalid;
    invalid.width = 320; invalid.height = 180; invalid.videoCodecName = "mpeg4";
    invalid.videoBitrateBits = 1500000;
    invalid.outputPath = temp.filePath("invalid.mkv").toUtf8().toStdString();
    invalid.subtitles = expected;
    libavcore::FrameEncoder rejected;
    g3 &= rejected.open(invalid).has_value();
    gate(3, g3);

    Streams bothStreams;
    const QString bothPath = temp.filePath("both.mp4");
    bool g4 = ready && render(timeline, bothPath, source, settings(Mode::Both))
        && inspect(bothPath, bothStreams) && matches(bothStreams, expected);
    const QImage both = libavcore::grabFrameAt(bothPath, 0.5);
    const QImage burn = libavcore::grabFrameAt(temp.filePath("normal-default.mp4"), 0.5);
    const QImage plain = libavcore::grabFrameAt(temp.filePath("embedded.mp4"), 0.5);
    gate(4, g4 && !both.isNull() && both == burn && both != plain);

    QFile mainSource("src/MainWindow.cpp");
    bool g5 = mainSource.open(QIODevice::ReadOnly);
    const QByteArray code = mainSource.readAll();
    g5 &= code.contains("exportCfg.appendSubtitleSettings(cfg);")
        && code.contains("cfg.appendSubtitleSettings(jcfg);")
        && code.contains("RenderQueue::appendSubtitleTrack(cfg, m_subtitleSegments, m_subtitleStyle, this);")
        && code.contains("RenderQueue::appendSubtitleTrack(jcfg, m_subtitleSegments, m_subtitleStyle, this);");
    QLineEdit *outputEdit = nullptr;
    for (auto *edit : dialog.findChildren<QLineEdit *>())
        if (edit->placeholderText() == "Select output file...") outputEdit = edit;
    g5 &= outputEdit && combo;
    for (Mode mode : {Mode::None, Mode::BurnIn, Mode::MovText, Mode::Both}) {
        if (outputEdit && combo) {
            outputEdit->setText(temp.filePath("gui.mp4"));
            combo->setCurrentIndex(static_cast<int>(mode));
            g5 &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection)
                && dialog.config().subtitleMode == mode;
        }
        RenderQueue queue;
        RenderJob job; job.outputPath = temp.filePath("queued.mp4"); job.exportConfig = settings(mode);
        queue.addJob(job);
        const auto originalJobs = queue.jobs();
        const QString queueFile = temp.filePath("queue.json");
        RenderQueue restored;
        g5 &= queue.saveQueue(queueFile) && restored.loadQueue(queueFile);
        const auto jobs = restored.jobs();
        g5 &= jobs.size() == 1 && originalJobs.size() == 1
            && jobs.first().exportConfig == originalJobs.first().exportConfig
            && jobs.first().exportConfig.value("subtitleMode").toInt(-1) == static_cast<int>(mode);
    }
    gate(5, g5);

    const QString nonePath = temp.filePath("none.mp4");
    Streams noneStreams;
    const bool noneOk = ready && render(timeline, nonePath, source, settings(Mode::None)) && inspect(nonePath, noneStreams);
    const QImage none = libavcore::grabFrameAt(nonePath, 0.5);
    QJsonObject noInstallation;
    RenderQueue::appendSubtitleTrack(noInstallation, segments, style, nullptr);
    QImage noBurn(320, 180, QImage::Format_RGBA8888); noBurn.fill(Qt::black);
    const QImage unchanged = noBurn;
    RenderQueue::paintSubtitles(noBurn, noInstallation, 500000);
    gate(6, noneOk && noneStreams.total == 2 && noneStreams.subtitle == 0
        && !none.isNull() && none == plain && none != burn && noBurn == unchanged);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
