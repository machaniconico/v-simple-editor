#include "../ExportDialog.h"
#include "../ExportUserPresets.h"
#include "../Exporter.h"
#include "../MainWindow.h"
#include "../RenderQueue.h"
#include "../Timeline.h"
#include "../RenderInPlace.h"
#include "../libavcore/AudioExtract.h"
#include <QStandardPaths>
extern "C" {
#include <libavformat/avformat.h>
}
#include "SelftestGate.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <cstdio>
#include <vector>

double exporter_loudnessGainDb();

int runExportCfgSelftest()
{
    selftest::GateTally tally;
    ExportConfig config;
    config.outputPath = QStringLiteral("/tmp/export-cfg.mp4");
    config.container = QStringLiteral("mkv");
    config.width = 640;
    config.height = 360;
    config.fps = 24;
    config.videoCodec = QStringLiteral("libx265");
    config.useHardwareAccel = true;
    config.hwEncoder = QStringLiteral("qsv");
    config.maxFileSizeMB = 256;
    config.videoBitrate = 4500;
    config.audioCodec = QStringLiteral("libopus");
    config.audioBitrate = 128;
    config.rateControl = ExportConfig::RateControl::Crf;
    config.crf = 19;
    config.exportMarkedRangeOnly = true;
    config.autoLoudness = true;
    config.targetLufs = -23.0;
    config.hdr10 = true;
    config.hdrSettings.mode = QStringLiteral("hdr10");
    config.hdrSettings.masterDisplayLuminanceMin = 0.05;
    config.hdrSettings.masterDisplayLuminanceMax = 2000.0;
    config.hdrSettings.maxCll = 1500;
    config.hdrSettings.maxFall = 600;
    config.hdrSettings.previewToneMap = QStringLiteral("hable");
    config.proresProfile = 4;
    config.keepAlpha = true;
    config.subtitleMode = subtitle::ExportMode::Both;
    config.imageSequence = true;
    config.sequenceFolder = QStringLiteral("/tmp/frames");
    config.sequencePattern = QStringLiteral("shot_%04d.png");
    config.sequenceDigits = 4;
    config.sequenceStart = 17;
    config.title = QStringLiteral("title");
    config.author = QStringLiteral("author");
    config.copyright = QStringLiteral("copyright");
    config.comment = QStringLiteral("comment");
    config.audioOnly = true;

    const QJsonObject mapped = exportcfg::toJobJson(config);
    const QStringList expectedKeys{
        "width", "height", "fps", "videoCodec", "videoBitrate", "rateControl", "crf",
        "audioCodec", "audioBitrate", "exportMarkedRangeOnly", "autoLoudness", "targetLufs",
        "hdr10", "hdrMode", "proresProfile", "keepAlpha", "subtitleMode",
        "imageSequence", "sequenceFolder", "sequencePattern", "sequenceDigits", "sequenceStart",
        "title", "author", "copyright", "comment"};
    const QStringList defaultKeys{"width", "height", "fps", "videoCodec", "videoBitrate",
        "audioCodec", "audioBitrate", "exportMarkedRangeOnly", "autoLoudness", "targetLufs",
        "subtitleMode"};
    const QJsonObject defaults = exportcfg::toJobJson(ExportConfig{});
    const QStringList mappedKeys = mapped.keys();
    const QStringList actualDefaultKeys = defaults.keys();
    const bool keySet = QSet<QString>(mappedKeys.begin(), mappedKeys.end()) ==
            QSet<QString>(expectedKeys.begin(), expectedKeys.end());
    const bool defaultSet = QSet<QString>(actualDefaultKeys.begin(), actualDefaultKeys.end()) ==
            QSet<QString>(defaultKeys.begin(), defaultKeys.end());
    const QJsonObject expectedValues{
        {"width", 640}, {"height", 360}, {"fps", 24},
        {"videoCodec", "libx265"}, {"videoBitrate", 4500},
        {"rateControl", "crf"}, {"crf", 19},
        {"audioCodec", "libopus"}, {"audioBitrate", 128},
        {"exportMarkedRangeOnly", true}, {"autoLoudness", true}, {"targetLufs", -23.0},
        {"hdr10", true}, {"hdrMode", "hdr10"}, {"proresProfile", 4},
        {"keepAlpha", true}, {"subtitleMode", static_cast<int>(subtitle::ExportMode::Both)},
        {"imageSequence", true}, {"sequenceFolder", "/tmp/frames"},
        {"sequencePattern", "shot_%04d.png"}, {"sequenceDigits", 4}, {"sequenceStart", 17},
        {"title", "title"}, {"author", "author"},
        {"copyright", "copyright"}, {"comment", "comment"}};
    const bool values = mapped == expectedValues;
    const QJsonObject expectedDefaults{
        {"width", 1920}, {"height", 1080}, {"fps", 30},
        {"videoCodec", "libx264"}, {"videoBitrate", 10000},
        {"audioCodec", "aac"}, {"audioBitrate", 192},
        {"exportMarkedRangeOnly", false}, {"autoLoudness", false},
        {"targetLufs", -14.0},
        {"subtitleMode", static_cast<int>(subtitle::ExportMode::BurnIn)}};
    const bool defaultValues = defaults == expectedDefaults;
    std::fprintf(stderr, "export-cfg G1 keySet=%d defaultSet=%d values=%d defaultValues=%d mapped=%lld defaults=%lld\n",
        keySet, defaultSet, values, defaultValues,
        static_cast<long long>(mapped.size()), static_cast<long long>(defaults.size()));
    tally.gate(1, keySet && defaultSet && values && defaultValues);

    // Exercise queue persistence (the RenderQueue consumer) and the preset
    // serializer's read path with the same non-default ExportConfig.
    QTemporaryDir temp;
    RenderQueue queue;
    RenderJob queued;
    queued.name = QStringLiteral("export-cfg");
    queued.outputPath = temp.filePath(QStringLiteral("output.mp4"));
    queued.width = config.width;
    queued.height = config.height;
    queued.codec = config.videoCodec;
    queued.bitrateBps = static_cast<qint64>(config.videoBitrate) * 1000;
    queued.exportConfig = mapped;
    queue.addJob(queued);
    RenderQueue loaded;
    const bool persisted = temp.isValid() && queue.saveQueue(temp.filePath("queue.json"))
        && loaded.loadQueue(temp.filePath("queue.json"))
        && loaded.jobs().size() == 1
        && loaded.jobs().first().exportConfig == mapped;
    const ExportConfig fromJob = ExportUserPresets::fromJson(mapped);
    const bool presetRoundtrip = fromJob.videoCodec == config.videoCodec
        && fromJob.audioCodec == config.audioCodec
        && fromJob.videoBitrate == config.videoBitrate
        && fromJob.audioBitrate == config.audioBitrate
        && fromJob.width == config.width && fromJob.height == config.height
        && fromJob.fps == config.fps
        && fromJob.rateControl == config.rateControl && fromJob.crf == config.crf
        && fromJob.hdr10 == config.hdr10 && fromJob.proresProfile == config.proresProfile
        && fromJob.keepAlpha == config.keepAlpha && fromJob.title == config.title
        && fromJob.exportMarkedRangeOnly == config.exportMarkedRangeOnly;
    // RenderQueue's mov_text reader must consume the non-default mode and the
    // serialized subtitle track, rather than re-reading the values above.
    SubtitleSegment cue;
    cue.startTime = 0.25;
    cue.endTime = 0.75;
    cue.text = QStringLiteral("export-cfg cue");
    QJsonObject subtitleConfig = mapped;
    RenderQueue::appendSubtitleTrack(subtitleConfig, QVector<SubtitleSegment>{cue}, SubtitleStyle{}, nullptr);
    const auto cues = RenderQueue::subtitleCues(subtitleConfig, 0, 1000000);
    const bool subtitleReader = cues.size() == 1
        && cues.front().startUs == 250000 && cues.front().endUs == 750000
        && cues.front().text == "export-cfg cue";
    QJsonObject burnOnly = subtitleConfig;
    burnOnly["subtitleMode"] = static_cast<int>(subtitle::ExportMode::BurnIn);
    const bool burnOnlyHasNoEmbeddedCue = RenderQueue::subtitleCues(burnOnly, 0, 1000000).empty();
    QJsonObject nonSequence = mapped;
    nonSequence["imageSequence"] = false;
    const bool queueReader = RenderQueue::alphaExportError(mapped).isEmpty()
        && !RenderQueue::alphaExportError(nonSequence).isEmpty();
    std::fprintf(stderr, "export-cfg G2 persisted=%d presetRoundtrip=%d subtitleReader=%d "
        "burnOnlyHasNoEmbeddedCue=%d cueCount=%lld cueStart=%lld cueEnd=%lld queueReader=%d\n",
        persisted, presetRoundtrip, subtitleReader, burnOnlyHasNoEmbeddedCue,
        static_cast<long long>(cues.size()),
        cues.empty() ? -1LL : static_cast<long long>(cues.front().startUs),
        cues.empty() ? -1LL : static_cast<long long>(cues.front().endUs), queueReader);
    tally.gate(2, persisted && presetRoundtrip && subtitleReader && burnOnlyHasNoEmbeddedCue && queueReader);

    MainWindow window;
    window.m_subtitleSegments.append(cue);
    const RenderJob normal = window.buildExportRenderJob(config, MainWindow::ExportEntry::Normal);
    const RenderJob mobile = window.buildExportRenderJob(config, MainWindow::ExportEntry::MobileBatch);
    bool containsMapped = true;
    for (auto it = mapped.begin(); it != mapped.end(); ++it) {
        containsMapped &= normal.exportConfig.value(it.key()) == it.value()
            && mobile.exportConfig.value(it.key()) == it.value();
    }
    const bool selectedDimensions = normal.width == config.width && normal.height == config.height
        && mobile.width == config.width && mobile.height == config.height
        && normal.exportConfig.value("width").toInt() == config.width
        && normal.exportConfig.value("height").toInt() == config.height
        && mobile.exportConfig.value("width").toInt() == config.width
        && mobile.exportConfig.value("height").toInt() == config.height;
    const auto normalCues = RenderQueue::subtitleCues(normal.exportConfig, 0, 1000000);
    const auto mobileCues = RenderQueue::subtitleCues(mobile.exportConfig, 0, 1000000);
    const bool subtitles = normal.exportConfig.value("subtitleMode").toInt()
            == static_cast<int>(subtitle::ExportMode::Both)
        && mobile.exportConfig.value("subtitleMode").toInt()
            == static_cast<int>(subtitle::ExportMode::Both)
        && !normal.exportConfig.value("subtitleTrack").toObject().isEmpty()
        && normal.exportConfig.value("subtitleTrack") == mobile.exportConfig.value("subtitleTrack")
        && normalCues.size() == 1 && mobileCues.size() == 1
        && normalCues.front().text == "export-cfg cue"
        && mobileCues.front().text == "export-cfg cue";
    const bool dynamic = normal.exportConfig.contains("timecodeBurnIn")
        && normal.exportConfig.contains("loudnessGainDb")
        && normal.loudnessGainDb == mobile.loudnessGainDb;
    const bool equalSelected = normal.exportConfig == mobile.exportConfig;

    window.m_projectConfig.explicitOutputResolution = true;
    window.m_projectConfig.width = 854;
    window.m_projectConfig.height = 480;
    const RenderJob explicitNormal = window.buildExportRenderJob(config, MainWindow::ExportEntry::Normal);
    const RenderJob explicitMobile = window.buildExportRenderJob(config, MainWindow::ExportEntry::MobileBatch);
    QJsonObject normalCommon = explicitNormal.exportConfig;
    QJsonObject mobileCommon = explicitMobile.exportConfig;
    normalCommon.remove("width");
    normalCommon.remove("height");
    mobileCommon.remove("width");
    mobileCommon.remove("height");
    const bool projectOverride = explicitNormal.width == 854 && explicitNormal.height == 480
        && explicitNormal.exportConfig.value("width").toInt() == 854
        && explicitNormal.exportConfig.value("height").toInt() == 480
        && explicitMobile.width == config.width && explicitMobile.height == config.height
        && explicitMobile.exportConfig.value("width").toInt() == config.width
        && explicitMobile.exportConfig.value("height").toInt() == config.height
        && explicitMobile.exportConfig == mobile.exportConfig
        && normalCommon == mobileCommon;
    std::fprintf(stderr, "export-cfg G3 containsMapped=%d selectedDimensions=%d subtitles=%d "
        "normalCues=%lld mobileCues=%lld dynamic=%d equalSelected=%d projectOverride=%d "
        "normal=%dx%d mobile=%dx%d explicitNormal=%dx%d explicitMobile=%dx%d\n",
        containsMapped, selectedDimensions, subtitles,
        static_cast<long long>(normalCues.size()), static_cast<long long>(mobileCues.size()),
        dynamic, equalSelected, projectOverride, normal.width, normal.height, mobile.width,
        mobile.height, explicitNormal.width, explicitNormal.height, explicitMobile.width, explicitMobile.height);
    tally.gate(3, containsMapped && selectedDimensions && subtitles && dynamic
        && equalSelected && projectOverride);

    // G4: loudnessauto::prepareJob runs inside buildExportRenderJob for BOTH entries.
    // Detects a regression where either route (or the shared builder) stops preparing:
    // the call counter then rises by less than 2 and loudnessAutoStatus is missing.
    // (buildExportRenderJob always writes exportConfig["loudnessGainDb"], so that key
    // alone cannot prove preparation; loudnessAutoStatus is written only by prepareJob.)
    ExportConfig on = config;
    on.autoLoudness = true;
    const int countBeforeOn = loudnessauto::prepareJobCallCountForTest();
    const RenderJob onNormal = window.buildExportRenderJob(on, MainWindow::ExportEntry::Normal);
    const RenderJob onMobile = window.buildExportRenderJob(on, MainWindow::ExportEntry::MobileBatch);
    const int onDelta = loudnessauto::prepareJobCallCountForTest() - countBeforeOn;
    const bool normalPrepared = onNormal.exportConfig.contains("loudnessAutoStatus")
        && (onNormal.exportConfig.contains("loudnessGainDb") || onNormal.exportConfig.contains("loudnessAutoError"));
    const bool mobilePrepared = onMobile.exportConfig.contains("loudnessAutoStatus")
        && (onMobile.exportConfig.contains("loudnessGainDb") || onMobile.exportConfig.contains("loudnessAutoError"));
    std::fprintf(stderr, "export-cfg G4 countDelta=%d normal[status=%d gain=%d error=%d] "
        "mobile[status=%d gain=%d error=%d]\n", onDelta,
        onNormal.exportConfig.contains("loudnessAutoStatus"), onNormal.exportConfig.contains("loudnessGainDb"),
        onNormal.exportConfig.contains("loudnessAutoError"),
        onMobile.exportConfig.contains("loudnessAutoStatus"), onMobile.exportConfig.contains("loudnessGainDb"),
        onMobile.exportConfig.contains("loudnessAutoError"));
    tally.gate(4, onDelta == 2 && normalPrepared && mobilePrepared);

    // G5: the out parameter returns the PRE-measurement project gain (what
    // RenderQueue::setLoudnessGainDb must keep receiving); job.loudnessGainDb holds the
    // measurement. The empty timeline measures 0 dB (no audio -> skip), the project
    // gain is set to -6 dB, so the two differ and a swap/overwrite is detected.
    const double previousProjectGain = exporter_loudnessGainDb();
    Exporter gainSetter;
    gainSetter.setLoudnessGainDb(-6.0);
    double normalProjectGain = 99.0;
    double mobileProjectGain = 99.0;
    const RenderJob gainNormal = window.buildExportRenderJob(on, MainWindow::ExportEntry::Normal, &normalProjectGain);
    const RenderJob gainMobile = window.buildExportRenderJob(on, MainWindow::ExportEntry::MobileBatch, &mobileProjectGain);
    gainSetter.setLoudnessGainDb(previousProjectGain);
    const bool projectGainReturned = normalProjectGain == -6.0 && mobileProjectGain == -6.0;
    const bool measuredInJob = gainNormal.loudnessGainDb == 0.0 && gainMobile.loudnessGainDb == 0.0
        && gainNormal.exportConfig.value("loudnessGainDb").toDouble(99.0) == gainNormal.loudnessGainDb
        && gainMobile.exportConfig.value("loudnessGainDb").toDouble(99.0) == gainMobile.loudnessGainDb;
    std::fprintf(stderr, "export-cfg G5 normal[project=%g job=%g cfg=%g] mobile[project=%g job=%g cfg=%g] "
        "previous=%g differ=%d\n",
        normalProjectGain, gainNormal.loudnessGainDb, gainNormal.exportConfig.value("loudnessGainDb").toDouble(99.0),
        mobileProjectGain, gainMobile.loudnessGainDb, gainMobile.exportConfig.value("loudnessGainDb").toDouble(99.0),
        previousProjectGain, normalProjectGain != gainNormal.loudnessGainDb);
    tally.gate(5, projectGainReturned && measuredInJob);

    // G6: autoLoudness OFF prepares nothing (counter unchanged, no prepare traces).
    ExportConfig off = config;
    off.autoLoudness = false;
    const int countBeforeOff = loudnessauto::prepareJobCallCountForTest();
    const RenderJob offNormal = window.buildExportRenderJob(off, MainWindow::ExportEntry::Normal);
    const RenderJob offMobile = window.buildExportRenderJob(off, MainWindow::ExportEntry::MobileBatch);
    const int offDelta = loudnessauto::prepareJobCallCountForTest() - countBeforeOff;
    const auto traces = [](const RenderJob &job) {
        return job.exportConfig.contains("loudnessAutoStatus") || job.exportConfig.contains("loudnessAutoError")
            || job.exportConfig.contains("measuredLufs");
    };
    std::fprintf(stderr, "export-cfg G6 countDelta=%d normalTraces=%d mobileTraces=%d "
        "normalTemp=%d mobileTemp=%d\n", offDelta, traces(offNormal), traces(offMobile),
        offNormal.exportConfig.contains("tempFiles"), offMobile.exportConfig.contains("tempFiles"));
    tally.gate(6, offDelta == 0 && !traces(offNormal) && !traces(offMobile)
        && !offNormal.exportConfig.contains("tempFiles") && !offMobile.exportConfig.contains("tempFiles"));
    // G7: the AAC intermediate's -b:a comes from the configured kbps (pure function, no
    // ffmpeg). Detects a regression back to the hard-coded 192k (128 would still say 192k).
    const QStringList args128 = exportaudio::aacAudioArgs(128);
    const QStringList args192 = exportaudio::aacAudioArgs(ExportConfig().audioBitrate);
    const QStringList argsBad = exportaudio::aacAudioArgs(0);
    const auto bitrateOf = [](const QStringList &args) {
        const int at = args.indexOf(QStringLiteral("-b:a"));
        return at >= 0 && at + 1 < args.size() ? args.at(at + 1) : QString();
    };
    std::fprintf(stderr, "export-cfg G7 args128=[%s] args192=[%s] args0=[%s] default=%d\n",
        qPrintable(args128.join(' ')), qPrintable(args192.join(' ')), qPrintable(argsBad.join(' ')),
        ExportConfig().audioBitrate);
    tally.gate(7, bitrateOf(args128) == QStringLiteral("128k") && bitrateOf(args192) == QStringLiteral("192k")
        && bitrateOf(argsBad) == QStringLiteral("192k") && args128.contains(QStringLiteral("aac")));

    // G8: the muxPath prepareJob converts to is registered in exportConfig["tempFiles"] and
    // RenderQueue::removeTempFiles deletes it. No ffmpeg (no intermediate created) -> SKIP.
    {
        QTemporaryDir g8Dir;
        const QString tone = g8Dir.filePath(QStringLiteral("tone.wav"));
        // Noise, so the AAC encoder actually spends the configured bitrate (G11).
        std::vector<float> samples(48000 * 2 * 2);
        unsigned noiseState = 12345u;
        for (float &sample : samples) {
            noiseState = noiseState * 1664525u + 1013904223u;
            sample = (static_cast<float>(noiseState >> 8) / 8388608.0f - 1.0f) * 0.3f;
        }
        QString wavError;
        const bool wavOk = g8Dir.isValid()
            && libavcore::writeWavFloatInterleaved(tone, samples, 48000, 2, &wavError);
        MainWindow g8Window;
        ClipInfo audioClip;
        audioClip.filePath = tone;
        audioClip.duration = audioClip.outPoint = 2.0;
        ClipInfo pictureClip;
        pictureClip.filePath = g8Dir.filePath(QStringLiteral("picture.mp4"));
        pictureClip.duration = pictureClip.outPoint = 2.0;
        g8Window.m_timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{pictureClip}},
            QVector<QVector<ClipInfo>>{{audioClip}}, 0, -1, -1, 10);
        ExportConfig g8Config = config;
        g8Config.outputPath = g8Dir.filePath(QStringLiteral("g8.mp4"));
        g8Config.autoLoudness = true;
        g8Config.audioBitrate = 128;
        RenderJob g8 = g8Window.buildExportRenderJob(g8Config, MainWindow::ExportEntry::Normal);
        QStringList created;
        for (const QJsonValue &value : g8.exportConfig.value("tempFiles").toArray())
            created << value.toString();
        bool createdExists = !created.isEmpty();
        for (const QString &path : created) createdExists = createdExists && QFile::exists(path);
        const bool muxIsProject = !g8.projectFilePath.isEmpty() && created.contains(g8.projectFilePath);
        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty()) {
            std::fprintf(stderr, "export-cfg G8 ffmpeg=none wavOk=%d created=%lld\n",
                wavOk, static_cast<long long>(created.size()));
            tally.skip(8, "ffmpeg.exe unavailable");
        } else {
            RenderQueue queue;
            queue.removeTempFiles(&g8);
            bool leftover = false;
            for (const QString &path : created) leftover = leftover || QFile::exists(path);
            std::fprintf(stderr, "export-cfg G8 created=%lld createdExists=%d muxIsProject=%d "
                "leftover=%d tempFilesKey=%d\n", static_cast<long long>(created.size()),
                createdExists, muxIsProject, leftover, g8.exportConfig.contains("tempFiles"));
            tally.gate(8, wavOk && createdExists && muxIsProject && !leftover
                && !g8.exportConfig.contains("tempFiles"));
        }

        // G12: a mix that fails after ffmpeg wrote the output leaves no file behind and returns
        // an empty path. The hook turns a successful write into a failure so the output really
        // exists when the cleanup runs (existedBefore). Without the cleanup in
        // prepareTimelineAudioMixForExport the file survives and this gate fails.
        if (ffmpeg.isEmpty()) {
            tally.skip(12, "ffmpeg.exe unavailable");
        } else {
            const QString failOut = g8Dir.filePath(QStringLiteral("fail-mix.m4a"));
            QString failError;
            exportaudio::outputExistedBeforeCleanupForTest = false;
            exportaudio::failAfterWriteForTest = true;
            const QString failed = renderinplace::prepareAudioMix(g8Window.m_timeline, failOut, 2.0, &failError);
            exportaudio::failAfterWriteForTest = false;
            const bool existedBefore = exportaudio::outputExistedBeforeCleanupForTest;
            const bool leftover = QFile::exists(failOut);
            const QString okMix = renderinplace::prepareAudioMix(g8Window.m_timeline, failOut, 2.0, &failError);
            const bool okKept = okMix == failOut && QFile::exists(failOut);
            std::fprintf(stderr, "export-cfg G12 returned=[%s] existedBefore=%d leftover=%d "
                "successKept=%d error=[%s]\n", qPrintable(failed), existedBefore, leftover,
                okKept, qPrintable(failError));
            tally.gate(12, failed.isEmpty() && existedBefore && !leftover && okKept);
        }

        // G11: the batch queue hands task.audioBitrateKbps to renderinplace::prepareAudioMix
        // (BatchExportQueue.cpp). Detects the batch AAC intermediate falling back to the
        // default 192k: the 128 kbps intermediate must probe near 128000 and below the
        // default-built one. No ffmpeg -> SKIP.
        if (ffmpeg.isEmpty()) {
            tally.skip(11, "ffmpeg.exe unavailable");
        } else {
            const auto probeBitRate = [](const QString &path) -> long long {
                AVFormatContext *format = nullptr;
                if (avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) < 0)
                    return -1;
                long long rate = -1;
                if (avformat_find_stream_info(format, nullptr) >= 0) {
                    for (unsigned i = 0; i < format->nb_streams; ++i)
                        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                            rate = format->streams[i]->codecpar->bit_rate;
                }
                avformat_close_input(&format);
                return rate;
            };
            const QString mix128 = g8Dir.filePath(QStringLiteral("batch128.m4a"));
            const QString mixDefault = g8Dir.filePath(QStringLiteral("batchdefault.m4a"));
            QString error128, errorDefault;
            const QString made128 = renderinplace::prepareAudioMix(
                g8Window.m_timeline, mix128, 2.0, &error128, 128);
            const QString madeDefault = renderinplace::prepareAudioMix(
                g8Window.m_timeline, mixDefault, 2.0, &errorDefault);
            const long long rate128 = made128 == mix128 ? probeBitRate(mix128) : -1;
            const long long rateDefault = madeDefault == mixDefault ? probeBitRate(mixDefault) : -1;
            std::fprintf(stderr, "export-cfg G11 made128=%d madeDefault=%d rate128=%lld rateDefault=%lld "
                "err128=[%s] errDefault=[%s]\n", made128 == mix128, madeDefault == mixDefault,
                rate128, rateDefault, qPrintable(error128), qPrintable(errorDefault));
            tally.gate(11, rate128 > 115000 && rate128 < 141000 && rateDefault > 170000
                && rate128 < rateDefault);
        }
    }

    // G9 / G10: choosing the image-sequence format disables the auto-loudness checkbox and
    // its sub-controls (restored on returning to video); config() keeps ignoring it there.
    // Detects removing updateLoudnessControlsEnabled's call from the format-change handler.
    {
        ExportDialog dialog(ProjectConfig{});
        auto *type = dialog.findChild<QComboBox *>("exportType");
        auto *toggle = dialog.findChild<QCheckBox *>("autoLoudness");
        auto *preset = dialog.findChild<QComboBox *>("loudnessPreset");
        auto *target = dialog.findChild<QDoubleSpinBox *>("targetLufs");
        const int videoIndex = type ? type->findData(static_cast<int>(ExportType::Video)) : -1;
        const int sequenceIndex = type ? type->findData(static_cast<int>(ExportType::ImageSequence)) : -1;
        const bool found = type && toggle && preset && target && videoIndex >= 0 && sequenceIndex >= 0;
        bool videoState[3] = {false, false, false}, sequenceState[3] = {true, true, true};
        bool backState[3] = {false, false, false};
        bool sequenceAuto = true, videoAuto = false;
        if (found) {
            type->setCurrentIndex(videoIndex);
            toggle->setChecked(true);
            const auto read = [&](bool *out) {
                out[0] = toggle->isEnabled(); out[1] = preset->isEnabled(); out[2] = target->isEnabled();
            };
            read(videoState);
            videoAuto = dialog.currentConfigForTest().autoLoudness;
            type->setCurrentIndex(sequenceIndex);
            read(sequenceState);
            sequenceAuto = dialog.currentConfigForTest().autoLoudness;
            type->setCurrentIndex(videoIndex);
            read(backState);
        }
        std::fprintf(stderr, "export-cfg G9 found=%d video[box=%d preset=%d target=%d] "
            "sequence[box=%d preset=%d target=%d] back[box=%d preset=%d target=%d]\n", found,
            videoState[0], videoState[1], videoState[2], sequenceState[0], sequenceState[1],
            sequenceState[2], backState[0], backState[1], backState[2]);
        tally.gate(9, found && videoState[0] && videoState[1] && videoState[2]
            && !sequenceState[0] && !sequenceState[1] && !sequenceState[2]
            && backState[0] && backState[1] && backState[2]);
        std::fprintf(stderr, "export-cfg G10 checked=%d videoAutoLoudness=%d sequenceAutoLoudness=%d\n",
            found && toggle->isChecked(), videoAuto, sequenceAuto);
        tally.gate(10, found && toggle->isChecked() && videoAuto && !sequenceAuto);
    }
    return tally.finish("export-cfg");
}
