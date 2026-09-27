#include "../CodecDetector.h"
#include "../ExportUserPresets.h"
#include "SelftestGate.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <array>
#include <cstdio>

namespace {
struct CheckResults {
    std::array<bool, 5> gates{};
    QString av1Reason;
    bool av1Available = false;
    bool decoysPresent = true;
};

bool addDecoys(ExportDialog &dialog, bool enabled)
{
    if (!enabled) return true;
    auto *spin = new QSpinBox(&dialog);
    spin->setObjectName("decoy");
    auto *combo = new QComboBox(&dialog);
    combo->setObjectName("decoy");
    return spin->suffix().isEmpty() && dialog.findChild<QSpinBox *>("decoy") == spin
        && dialog.findChild<QComboBox *>("decoy") == combo;
}

CheckResults runChecks(bool withDecoys)
{
    CheckResults results;
    const auto gate = [&](int n, bool ok) { results.gates.at(n - 1) = ok; };
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
    const bool alphaRead = config.keepAlpha;
    const bool alphaDefault = !ExportUserPresets::fromJson({}).keepAlpha;
    const bool alphaOmitted = !ExportUserPresets::toJson(ExportConfig{}).contains("keepAlpha");
    const bool jsonMatched = json == expected;
    const bool outputOmitted = !json.contains("outputPath");
    const bool inputPathIgnored = ExportUserPresets::fromJson(withPath).outputPath.isEmpty();
    const bool roundTrip = ExportUserPresets::toJson(ExportUserPresets::fromJson(json)) == expected;
    std::fprintf(stderr, "export-presets%s G1: alphaRead=%d alphaDefault=%d alphaOmitted=%d json=%d outputOmitted=%d inputPathIgnored=%d roundTrip=%d\n",
        withDecoys ? " decoy" : "", alphaRead, alphaDefault, alphaOmitted,
        jsonMatched, outputOmitted, inputPathIgnored, roundTrip);
    gate(1, alphaRead && alphaDefault && alphaOmitted && jsonMatched
        && outputOmitted && inputPathIgnored && roundTrip);

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
    std::fprintf(stderr, "export-presets%s G2: saved=%d matched=%d safe=%d files=%lld\n",
        withDecoys ? " decoy" : "", saved, matched, safe,
        static_cast<long long>(files.size()));
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
        results.decoysPresent &= addDecoys(dialog, withDecoys);
        auto *presetCombo = dialog.findChild<QComboBox *>("preset");
        auto *audioOnly = dialog.findChild<QCheckBox *>("audioOnly");
        auto *output = dialog.findChild<QLineEdit *>("outputPath");
        auto *crf = dialog.findChild<QSpinBox *>("crf");
        auto *videoBitrate = dialog.findChild<QSpinBox *>("videoBitrate");
        auto *audioBitrate = dialog.findChild<QSpinBox *>("audioBitrate");
        restored &= presetCombo && audioOnly && output;
        restored &= crf && videoBitrate && audioBitrate;
        if (presetCombo && audioOnly && output && crf && videoBitrate && audioBitrate) {
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
                    const bool userMode = audioOnly->isChecked() && !presetCombo->isEnabled();
                    presetCombo->setCurrentIndex(index);
                    const bool builtinMode = !audioOnly->isChecked() && presetCombo->isEnabled();
                    const bool outputSuffix = QFileInfo(output->text()).suffix() != "wav";
                    const bool accepted = QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
                    const auto selected = dialog.config();
                    const bool hdrReset = ExportUserPresets::toJson(selected).value("hdrSettings")
                        == ExportUserPresets::toJson(ExportConfig{}).value("hdrSettings");
                    const bool videoMode = !selected.audioOnly
                        && QStringList{"mp4", "mkv", "webm", "mov"}.contains(selected.container);
                    const bool crfRange = crf->minimum() == 0
                        && crf->maximum() == (selected.videoCodec.contains("av1") ? 63 : 51);
                    const bool videoRange = videoBitrate->minimum() == 500
                        && videoBitrate->maximum() == 100000;
                    const bool audioRange = audioBitrate->minimum() == 64
                        && audioBitrate->maximum() == 512;
                    const bool proresBounds = names[index] != "ProRes 4444"
                        || (selected.audioBitrate == 512 && selected.videoBitrate == 100000);
                    const bool selectionOk = userMode && builtinMode && outputSuffix && accepted
                        && hdrReset && videoMode && crfRange && videoRange && audioRange
                        && proresBounds;
                    std::fprintf(stderr, "export-presets%s preset[%d]: selected=%d codec=%s container=%s audioOnly=%d userMode=%d builtinMode=%d outputSuffix=%d accepted=%d hdrReset=%d videoMode=%d crfRange=%d videoRange=%d audioRange=%d proresBounds=%d\n",
                        withDecoys ? " decoy" : "",
                        index, selectionOk, qPrintable(selected.videoCodec), qPrintable(selected.container),
                        selected.audioOnly, userMode, builtinMode, outputSuffix, accepted,
                        hdrReset, videoMode, crfRange, videoRange, audioRange, proresBounds);
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
            results.decoysPresent &= addDecoys(dialog, withDecoys);
            auto *presetCombo = dialog.findChild<QComboBox *>("preset");
            auto *rateControl = dialog.findChild<QComboBox *>("rateControl");
            auto *crf = dialog.findChild<QSpinBox *>("crf");
            auto *output = dialog.findChild<QLineEdit *>("outputPath");
            crfOk &= presetCombo && rateControl && crf && output;
            if (!presetCombo || !rateControl || !crf || !output) {
                std::fprintf(stderr, "export-presets%s CRF[%d,%d]: preset=%d rateControl=%d crf=%d output=%d\n",
                    withDecoys ? " decoy" : "", builtinIndex, phase,
                    presetCombo != nullptr, rateControl != nullptr, crf != nullptr,
                    output != nullptr);
                continue;
            }
            output->setText(temp.filePath("crf-export.mp4"));
            presetCombo->setCurrentIndex(builtinIndex);
            if (phase == 0) {
                // Same-codec built-in changes must preserve manually entered CRF.
                rateControl->setCurrentIndex(1);
                crf->setValue(27);
                presetCombo->setCurrentIndex(names.size() - 1); // Custom retains codec.
                presetCombo->setCurrentIndex(builtinIndex);
                const bool preserved = crf->value() == 27;
                rateControl->setCurrentIndex(0);
                const bool accepted = QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
                const auto bitrateConfig = dialog.config();
                const bool unsetCrf = bitrateConfig.rateControl == ExportConfig::RateControl::Bitrate
                    && bitrateConfig.crf == -1;
                const bool savedCrf = ExportUserPresets::save({savedName, bitrateConfig});
                crfOk &= preserved && accepted && unsetCrf && savedCrf;
                std::fprintf(stderr, "export-presets%s CRF[%d,0]: preserved=%d accepted=%d unset=%d saved=%d\n",
                    withDecoys ? " decoy" : "", builtinIndex, preserved, accepted,
                    unsetCrf, savedCrf);
            } else {
                const int userIndex = presetCombo->findData(savedName);
                crfOk &= userIndex >= 0;
                if (userIndex < 0) {
                    std::fprintf(stderr, "export-presets%s CRF[%d,1]: userIndex=%d\n",
                        withDecoys ? " decoy" : "", builtinIndex, userIndex);
                    continue;
                }
                presetCombo->setCurrentIndex(userIndex);
                const bool persistedUnset = crf->value() == -1;
                presetCombo->setCurrentIndex(builtinIndex);
                const bool defaulted = crf->minimum() == 0 && crf->value() == defaultCrf
                    && crf->maximum() == (builtinIndex == 9 ? 63 : 51);
                rateControl->setCurrentIndex(1);
                const bool accepted = QMetaObject::invokeMethod(&dialog, "onExport", Qt::DirectConnection);
                const bool exported = dialog.config().rateControl == ExportConfig::RateControl::Crf
                    && dialog.config().crf == defaultCrf;
                crfOk &= persistedUnset && defaulted && accepted && exported;
                std::fprintf(stderr, "export-presets%s CRF[%d,1]: persistedUnset=%d defaulted=%d accepted=%d exported=%d\n",
                    withDecoys ? " decoy" : "", builtinIndex, persistedUnset,
                    defaulted, accepted, exported);
            }
        }
        return crfOk;
    };
    const bool h264Crf = checkCrf(0);
    const bool nonAv1NamesMatch = actualNonAv1.size() == 19 && actualNonAv1 == expectedNonAv1;
    std::fprintf(stderr, "export-presets%s G3: nonAv1Count=%lld namesMatch=%d dialog=%d h264Crf=%d\n",
        withDecoys ? " decoy" : "", static_cast<long long>(actualNonAv1.size()),
        nonAv1NamesMatch, restored, h264Crf);
    gate(3, nonAv1NamesMatch && restored && h264Crf);
    ExportConfig resolved;
    QString error;
    const bool builtInSaved = ExportUserPresets::save({names.first(), config});
    const bool userSaved = ExportUserPresets::save({name, config});
    const bool builtInResolved = ExportUserPresets::resolve(names.first(), &resolved, &error);
    const bool builtInPreferred = resolved.videoCodec == "libx264" && !resolved.audioOnly;
    const bool userResolved = ExportUserPresets::resolve(name, &resolved, &error);
    const bool userMatched = ExportUserPresets::toJson(resolved) == expected;
    const bool missingRejected = !ExportUserPresets::resolve("missing", &resolved, &error);
    const bool errorMatched = error == QStringLiteral("プリセットが見つかりません: missing");
    const bool priority = builtInSaved && userSaved && builtInResolved && builtInPreferred
        && userResolved && userMatched && missingRejected && errorMatched;
    std::fprintf(stderr, "export-presets%s G4: builtInSaved=%d userSaved=%d builtInResolved=%d builtInPreferred=%d userResolved=%d userMatched=%d missingRejected=%d errorMatched=%d\n",
        withDecoys ? " decoy" : "", builtInSaved, userSaved, builtInResolved,
        builtInPreferred, userResolved, userMatched, missingRejected, errorMatched);
    gate(4, priority);
    results.av1Available = selftest::requireEncoder("libsvtav1", &results.av1Reason);
    if (results.av1Available) {
        const bool av1Crf = checkCrf(9);
        const bool av1Listed = actual.size() == 20 && actual.at(9) == names.at(9);
        std::fprintf(stderr, "export-presets%s G5: listed=%d selected=%d selection=%d av1Crf=%d\n",
            withDecoys ? " decoy" : "", av1Listed, checkedAv1, av1Selection, av1Crf);
        gate(5, av1Listed && checkedAv1 && av1Selection && av1Crf);
    }
    if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldEnv);
    else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    return results;
}
}

int runExportPresetsSelftest()
{
    selftest::GateTally tally;
    const CheckResults baseline = runChecks(false);
    for (int n = 1; n <= 4; ++n) tally.gate(n, baseline.gates.at(n - 1));
    if (baseline.av1Available) tally.gate(5, baseline.gates.at(4));
    else tally.skip(5, qPrintable(baseline.av1Reason));

    const CheckResults decoy = runChecks(true);
    const bool sameAvailability = decoy.av1Available == baseline.av1Available;
    const bool g6 = decoy.gates.at(0) && decoy.gates.at(1)
        && decoy.gates.at(2) && decoy.gates.at(3)
        && (!decoy.av1Available || decoy.gates.at(4)) && sameAvailability
        && decoy.decoysPresent;
    std::fprintf(stderr, "export-presets G6: g1=%d g2=%d g3=%d g4=%d g5=%s availabilitySame=%d decoysPresent=%d\n",
        decoy.gates.at(0), decoy.gates.at(1), decoy.gates.at(2), decoy.gates.at(3),
        decoy.av1Available ? (decoy.gates.at(4) ? "PASS" : "FAIL") : "SKIP",
        sameAvailability, decoy.decoysPresent);
    tally.gate(6, g6);
    return tally.finish();
}
