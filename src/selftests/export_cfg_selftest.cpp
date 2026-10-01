#include "../ExportDialog.h"
#include "../ExportUserPresets.h"
#include "../MainWindow.h"
#include "../RenderQueue.h"
#include "SelftestGate.h"
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <cstdio>

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
    return tally.finish("export-cfg");
}
