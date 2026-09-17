#include "../ExportUserPresets.h"
#include <QDir>
#include <QFileInfo>
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
    bool restored = ExportUserPresets::save({name, config});
    {
        ExportDialog dialog(ProjectConfig{});
        QComboBox *presetCombo = nullptr;
        QCheckBox *audioOnly = nullptr;
        for (auto *combo : dialog.findChildren<QComboBox *>())
            if (combo->findText(names.first()) >= 0) presetCombo = combo;
        for (auto *checkbox : dialog.findChildren<QCheckBox *>())
            if (checkbox->text() == QStringLiteral("音声のみ (映像なし)")) audioOnly = checkbox;
        QLineEdit *output = nullptr;
        for (auto *edit : dialog.findChildren<QLineEdit *>())
            if (edit->placeholderText() == QStringLiteral("Select output file...")) output = edit;
        restored &= presetCombo && audioOnly && output;
        if (presetCombo && audioOnly && output) {
            // Without using a user preset, audio-only mode must still lock the combo.
            audioOnly->setChecked(true);
            restored &= !presetCombo->isEnabled();
            audioOnly->setChecked(false);
            restored &= presetCombo->isEnabled();
            const int userIndex = presetCombo->findData(name);
            restored &= userIndex >= 0;
            if (userIndex >= 0) {
                output->setText(temp.filePath("export.wav"));
                presetCombo->setCurrentIndex(userIndex);
                restored &= audioOnly->isChecked();
                audioOnly->setChecked(false);
                restored &= QFileInfo(output->text()).suffix() == "mkv";
                restored &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
                restored &= !dialog.config().audioOnly && dialog.config().container == "mkv";

                // Programmatic selection also exercises the built-in reset while
                // audio-only mode disables interactive selection. Do not uncheck it.
                for (int index = 0; index < names.size(); ++index) {
                    presetCombo->setCurrentIndex(index);
                    presetCombo->setCurrentIndex(userIndex);
                    restored &= audioOnly->isChecked() && !presetCombo->isEnabled();
                    presetCombo->setCurrentIndex(index);
                    restored &= !audioOnly->isChecked() && presetCombo->isEnabled();
                    restored &= QFileInfo(output->text()).suffix() != "wav";
                    restored &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
                    const auto selected = dialog.config();
                    restored &= ExportUserPresets::toJson(selected).value("hdrSettings")
                        == ExportUserPresets::toJson(ExportConfig{}).value("hdrSettings");
                    restored &= !selected.audioOnly
                        && QStringList{"mp4", "mkv", "webm", "mov"}.contains(selected.container);
                    const auto spins = dialog.findChildren<QSpinBox *>();
                    restored &= spins.size() == 3;
                    for (auto *spin : spins) {
                        if (spin->suffix().isEmpty()) {
                            restored &= spin->minimum() == 0
                                && spin->maximum() == (selected.videoCodec.contains("av1") ? 63 : 51);
                        } else if (spin->singleStep() == 500) {
                            restored &= spin->minimum() == 500 && spin->maximum() == 100000;
                        } else {
                            restored &= spin->minimum() == 64 && spin->maximum() == 512;
                        }
                    }
                    if (names[index] == "ProRes 4444")
                        restored &= selected.audioBitrate == 512 && selected.videoBitrate == 100000;
                }
            }
        }
    }
    // Save actual bitrate-mode dialog settings, then reopen to exercise the
    // persisted unset CRF sentinel with both H.264 and AV1.
    for (const int builtinIndex : {0, 9}) {
        const int defaultCrf = builtinIndex == 9 ? 30 : 23;
        const QString savedName = QStringLiteral("未設定CRF-%1").arg(builtinIndex);
        for (int phase = 0; phase < 2; ++phase) {
            ExportDialog dialog(ProjectConfig{});
            QComboBox *presetCombo = nullptr;
            QComboBox *rateControl = nullptr;
            QSpinBox *crf = nullptr;
            QLineEdit *output = nullptr;
            for (auto *combo : dialog.findChildren<QComboBox *>()) {
                if (combo->findText(names.first()) >= 0) presetCombo = combo;
                if (combo->findText(QStringLiteral("品質 (CRF)")) >= 0) rateControl = combo;
            }
            for (auto *spin : dialog.findChildren<QSpinBox *>())
                if (spin->suffix().isEmpty()) crf = spin;
            for (auto *edit : dialog.findChildren<QLineEdit *>())
                if (edit->placeholderText() == QStringLiteral("Select output file...")) output = edit;
            restored &= presetCombo && rateControl && crf && output;
            if (!presetCombo || !rateControl || !crf || !output) continue;
            output->setText(temp.filePath("crf-export.mp4"));
            presetCombo->setCurrentIndex(builtinIndex);
            if (phase == 0) {
                // Same-codec built-in changes must preserve manually entered CRF.
                rateControl->setCurrentIndex(1);
                crf->setValue(27);
                presetCombo->setCurrentIndex(names.size() - 1); // Custom retains codec.
                presetCombo->setCurrentIndex(builtinIndex);
                restored &= crf->value() == 27;
                rateControl->setCurrentIndex(0);
                restored &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
                const auto bitrateConfig = dialog.config();
                restored &= bitrateConfig.rateControl == ExportConfig::RateControl::Bitrate
                    && bitrateConfig.crf == -1;
                restored &= ExportUserPresets::save({savedName, bitrateConfig});
            } else {
                const int userIndex = presetCombo->findData(savedName);
                restored &= userIndex >= 0;
                if (userIndex < 0) continue;
                presetCombo->setCurrentIndex(userIndex);
                restored &= crf->value() == -1;
                presetCombo->setCurrentIndex(builtinIndex);
                restored &= crf->minimum() == 0 && crf->value() == defaultCrf
                    && crf->maximum() == (builtinIndex == 9 ? 63 : 51);
                rateControl->setCurrentIndex(1);
                restored &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
                restored &= dialog.config().rateControl == ExportConfig::RateControl::Crf
                    && dialog.config().crf == defaultCrf;
            }
        }
    }
    gate(3, actual.size() == 20 && actual == names && restored);
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
