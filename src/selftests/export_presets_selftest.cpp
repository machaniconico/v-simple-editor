#include "../CodecDetector.h"
#include "../ExportUserPresets.h"
#include "SelftestGate.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>

int runExportPresetsSelftest()
{
    selftest::GateTally tally;
    const auto gate = [&](int n, bool ok) { tally.gate(n, ok); };
    const QJsonObject expected{
        {"rateControl", "crf"}, {"crf", 20}, {"videoCodec", "libx265"},
        {"audioCodec", "pcm_s16le"}, {"container", "wav"},
        {"videoBitrate", 35000}, {"audioBitrate", 1536},
        {"width", 2560}, {"height", 1440}, {"fps", 60},
        {"useHardwareAccel", true}, {"hwEncoder", "qsv"},
        {"maxFileSizeMB", 25}, {"hdr10", true}, {"proresProfile", 4}, {"keepAlpha", true},
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
    gate(1, config.keepAlpha && !ExportUserPresets::fromJson({}).keepAlpha
        && !ExportUserPresets::toJson(ExportConfig{}).contains("keepAlpha")
        && json == expected && !json.contains("outputPath")
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
    QStringList expectedNonAv1 = names;
    expectedNonAv1.removeAt(9);
    QStringList actualNonAv1 = actual;
    actualNonAv1.removeAll(names.at(9));
    bool restored = ExportUserPresets::save({name, config});
    bool av1Selection = true;
    bool checkedAv1 = false;
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
                    bool selectionOk = true;
                    presetCombo->setCurrentIndex(index);
                    presetCombo->setCurrentIndex(userIndex);
                    selectionOk &= audioOnly->isChecked() && !presetCombo->isEnabled();
                    presetCombo->setCurrentIndex(index);
                    selectionOk &= !audioOnly->isChecked() && presetCombo->isEnabled();
                    selectionOk &= QFileInfo(output->text()).suffix() != "wav";
                    selectionOk &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
                    const auto selected = dialog.config();
                    selectionOk &= ExportUserPresets::toJson(selected).value("hdrSettings")
                        == ExportUserPresets::toJson(ExportConfig{}).value("hdrSettings");
                    selectionOk &= !selected.audioOnly
                        && QStringList{"mp4", "mkv", "webm", "mov"}.contains(selected.container);
                    const auto spins = dialog.findChildren<QSpinBox *>();
                    selectionOk &= spins.size() == 3;
                    for (auto *spin : spins) {
                        if (spin->suffix().isEmpty()) {
                            selectionOk &= spin->minimum() == 0
                                && spin->maximum() == (selected.videoCodec.contains("av1") ? 63 : 51);
                        } else if (spin->singleStep() == 500) {
                            selectionOk &= spin->minimum() == 500 && spin->maximum() == 100000;
                        } else {
                            selectionOk &= spin->minimum() == 64 && spin->maximum() == 512;
                        }
                    }
                    if (names[index] == "ProRes 4444")
                        selectionOk &= selected.audioBitrate == 512 && selected.videoBitrate == 100000;
                    std::fprintf(stderr, "export-presets preset[%d]: selected=%d codec=%s container=%s audioOnly=%d spins=%lld\n",
                        index, selectionOk, qPrintable(selected.videoCodec), qPrintable(selected.container),
                        selected.audioOnly, static_cast<long long>(spins.size()));
                    if (index == 9) {
                        checkedAv1 = true;
                        av1Selection &= selectionOk;
                    } else {
                        restored &= selectionOk;
                    }
                }
            }
        }
    }
    // Save actual bitrate-mode dialog settings, then reopen to exercise the
    // persisted unset CRF sentinel with both H.264 and AV1.
    const auto checkCrf = [&](int builtinIndex) {
        bool crfOk = true;
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
            crfOk &= presetCombo && rateControl && crf && output;
            if (!presetCombo || !rateControl || !crf || !output) continue;
            output->setText(temp.filePath("crf-export.mp4"));
            presetCombo->setCurrentIndex(builtinIndex);
            if (phase == 0) {
                // Same-codec built-in changes must preserve manually entered CRF.
                rateControl->setCurrentIndex(1);
                crf->setValue(27);
                presetCombo->setCurrentIndex(names.size() - 1); // Custom retains codec.
                presetCombo->setCurrentIndex(builtinIndex);
                crfOk &= crf->value() == 27;
                rateControl->setCurrentIndex(0);
                crfOk &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
                const auto bitrateConfig = dialog.config();
                crfOk &= bitrateConfig.rateControl == ExportConfig::RateControl::Bitrate
                    && bitrateConfig.crf == -1;
                crfOk &= ExportUserPresets::save({savedName, bitrateConfig});
            } else {
                const int userIndex = presetCombo->findData(savedName);
                crfOk &= userIndex >= 0;
                if (userIndex < 0) continue;
                presetCombo->setCurrentIndex(userIndex);
                crfOk &= crf->value() == -1;
                presetCombo->setCurrentIndex(builtinIndex);
                crfOk &= crf->minimum() == 0 && crf->value() == defaultCrf
                    && crf->maximum() == (builtinIndex == 9 ? 63 : 51);
                rateControl->setCurrentIndex(1);
                crfOk &= QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
                crfOk &= dialog.config().rateControl == ExportConfig::RateControl::Crf
                    && dialog.config().crf == defaultCrf;
            }
        }
        return crfOk;
    };
    const bool h264Crf = checkCrf(0);
    const bool nonAv1NamesMatch = actualNonAv1.size() == 19 && actualNonAv1 == expectedNonAv1;
    std::fprintf(stderr, "export-presets G3: nonAv1Count=%lld namesMatch=%d dialog=%d h264Crf=%d\n",
        static_cast<long long>(actualNonAv1.size()), nonAv1NamesMatch, restored, h264Crf);
    gate(3, nonAv1NamesMatch && restored && h264Crf);
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
    QString av1Reason;
    if (selftest::requireEncoder("libsvtav1", &av1Reason)) {
        const bool av1Crf = checkCrf(9);
        const bool av1Listed = actual.size() == 20 && actual.at(9) == names.at(9);
        std::fprintf(stderr, "export-presets G5: listed=%d selected=%d selection=%d av1Crf=%d\n",
            av1Listed, checkedAv1, av1Selection, av1Crf);
        gate(5, av1Listed && checkedAv1 && av1Selection && av1Crf);
    } else {
        tally.skip(5, qPrintable(av1Reason));
    }
    if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
    else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    return tally.finish();
}
