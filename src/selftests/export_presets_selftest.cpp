#include "../ExportUserPresets.h"
#include <QDir>
#include <QTemporaryDir>
#include <cstdio>

int runExportPresetsSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    const QJsonObject expected{
        {"rateControl", "crf"}, {"crf", 20}, {"videoCodec", "libx265"},
        {"audioCodec", "pcm_s16le"}, {"container", "wav"},
        {"videoBitrate", 35000}, {"audioBitrate", 1536},
        {"width", 2560}, {"height", 1440}, {"fps", 60},
        {"useHardwareAccel", true}, {"hwEncoder", "qsv"},
        {"maxFileSizeMB", 25}, {"hdr10", true}, {"proresProfile", 4},
        {"exportMarkedRangeOnly", true}, {"audioOnly", true},
        {"hdrSettings", QJsonObject{{"mode", "hlg"},
            {"masterDisplayLuminanceMin", 0.02}, {"masterDisplayLuminanceMax", 2000.0},
            {"maxCll", 1800}, {"maxFall", 600}, {"previewToneMap", "hable"}}}
    };
    ExportConfig config = ExportUserPresets::fromJson(expected);
    config.outputPath = "never-save-this.wav";
    const auto json = ExportUserPresets::toJson(config);
    auto withPath = json;
    withPath.insert("outputPath", "ignored.wav");
    gate(1, json == expected && !json.contains("outputPath")
        && ExportUserPresets::fromJson(withPath).outputPath.isEmpty()
        && ExportUserPresets::toJson(ExportUserPresets::fromJson(json)) == expected);

    const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
    const QByteArray oldEnv = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
    QTemporaryDir temp;
    qputenv("VEDITOR_EXPORT_PRESET_DIR", temp.path().toUtf8());
    const QString name = QStringLiteral("../テスト/A:B");
    bool saved = temp.isValid() && ExportUserPresets::save({name, config})
        && ExportUserPresets::save({"../テスト/A?B", ExportConfig{}});
    const auto loaded = ExportUserPresets::load();
    bool matched = false;
    for (const auto &p : loaded)
        if (p.name == name) matched = ExportUserPresets::toJson(p.config) == expected;
    const auto files = QDir(temp.path()).entryList({"*.json"}, QDir::Files);
    bool safe = files.size() == 2;
    for (const auto &file : files) safe &= !file.contains('/') && !file.contains(':');
    saved &= ExportUserPresets::save({name, ExportConfig{}})
        && ExportUserPresets::load().size() == 2
        && ExportUserPresets::remove(name) && ExportUserPresets::load().size() == 1
        && ExportUserPresets::remove("../テスト/A?B") && ExportUserPresets::load().isEmpty();
    gate(2, saved && matched && safe);

    const QStringList names{
        "YouTube (1080p H.264)", "YouTube (1440p H.264)", "YouTube (4K H.264)",
        "HDR10 (HEVC Main10)", "ProRes 422 Proxy", "ProRes 422 LT", "ProRes 422",
        "ProRes 422 HQ", "ProRes 4444", "YouTube (AV1 高圧縮)", "YouTube Shorts",
        "TikTok / Reels", "X / Twitter", "Facebook", "Twitch Clip (60fps)",
        "Discord (25MB制限)", "ニコニコ動画", "H.265 高画質", "VP9 WebM", "Custom"};
    QStringList actual;
    for (const auto &p : ExportDialog::presets()) actual.append(p.name);
    gate(3, actual.size() == 20 && actual == names);
    ExportConfig resolved;
    QString error;
    bool priority = ExportUserPresets::save({names.first(), config})
        && ExportUserPresets::save({name, config})
        && ExportUserPresets::resolve(names.first(), &resolved, &error)
        && resolved.videoCodec == "libx264" && !resolved.audioOnly
        && ExportUserPresets::resolve(name, &resolved, &error)
        && ExportUserPresets::toJson(resolved) == expected
        && !ExportUserPresets::resolve("missing", &resolved, &error)
        && error == QStringLiteral("プリセットが見つかりません: missing");
    gate(4, priority);
    if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
    else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
