#include "../SubtitleTranslator.h"
#include "../WhisperTranscriber.h"

#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>

#include <iostream>

int runWhisperGuideSelftest()
{
    int passed = 0;
    int failed = 0;
    const auto gate = [&](int number, bool ok) {
        if (ok) {
            ++passed;
            std::cerr << "PASS G" << number << '\n';
        } else {
            ++failed;
            std::cerr << "FAIL G" << number << '\n';
        }
    };

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        for (int number = 1; number <= 5; ++number)
            gate(number, false);
        std::cerr << "summary: " << passed << " PASS, " << failed << " FAIL\n";
        return failed;
    }

    const QStringList candidates = {
        QStringLiteral("/test/home/.local/bin/whisper-cli"),
        QStringLiteral("/opt/homebrew/bin/whisper-cli"),
        QStringLiteral("/usr/local/bin/whisper-cli")
    };

    QSettings whisperSettings(tempDir.filePath(QStringLiteral("whisper.ini")),
                              QSettings::IniFormat);
    whisperSettings.setValue(QStringLiteral("whisper/cli_path"),
                             QStringLiteral("/configured/whisper-cli"));
    const whisperpath::Resolution settingsFirst = whisperpath::resolveWhisperCli(
        whisperSettings,
        QStringLiteral("/environment/whisper-cli"),
        QStringLiteral("/path/whisper-cli"),
        candidates,
        candidates);
    gate(1, settingsFirst.executablePath == QStringLiteral("/configured/whisper-cli"));

    whisperSettings.remove(QStringLiteral("whisper/cli_path"));
    const whisperpath::Resolution environmentFirst = whisperpath::resolveWhisperCli(
        whisperSettings,
        QStringLiteral("/environment/whisper-cli"),
        QStringLiteral("/path/whisper-cli"),
        candidates,
        candidates);
    gate(2, environmentFirst.executablePath == QStringLiteral("/environment/whisper-cli"));

    const whisperpath::Resolution missing = whisperpath::resolveWhisperCli(
        whisperSettings, QString(), QString(), candidates, QStringList());
    gate(3, missing.executablePath.isEmpty()
                && missing.candidatePaths == candidates);

    const QString savedPath = tempDir.filePath(QStringLiteral("translate-saved.ini"));
    {
        QSettings settings(savedPath, QSettings::IniFormat);
        subxlat::saveDialogSettings(settings,
                                    subxlat::Provider::DeepL,
                                    QStringLiteral("saved-secret"),
                                    true);
    }
    QSettings savedSettings(savedPath, QSettings::IniFormat);
    const bool savedRoundTrip =
        savedSettings.value(QStringLiteral("translate/api_key")).toString()
            == QStringLiteral("saved-secret")
        && subxlat::providerFromSettings(savedSettings) == subxlat::Provider::DeepL;

    const QString unsavedPath = tempDir.filePath(QStringLiteral("translate-unsaved.ini"));
    {
        QSettings settings(unsavedPath, QSettings::IniFormat);
        subxlat::saveDialogSettings(settings,
                                    subxlat::Provider::GoogleV2,
                                    QStringLiteral("must-not-be-saved"),
                                    false);
    }
    QSettings unsavedSettings(unsavedPath, QSettings::IniFormat);
    const bool stayedUnsaved =
        !unsavedSettings.contains(QStringLiteral("translate/api_key"))
        && subxlat::providerFromSettings(unsavedSettings)
            == subxlat::Provider::GoogleV2;
    gate(4, savedRoundTrip && stayedUnsaved);

    QSettings configSettings(tempDir.filePath(QStringLiteral("translate-config.ini")),
                             QSettings::IniFormat);
    configSettings.setValue(QStringLiteral("translate/api_key"),
                            QStringLiteral("settings-secret"));
    const subxlat::TranslateConfig envConfig =
        subxlat::TranslateConfig::defaultConfig(
            configSettings, QStringLiteral("environment-secret"));
    const subxlat::TranslateConfig settingsConfig =
        subxlat::TranslateConfig::defaultConfig(configSettings, QString());
    gate(5, envConfig.apiKey == QStringLiteral("environment-secret")
                && settingsConfig.apiKey == QStringLiteral("settings-secret"));

    std::cerr << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    return failed;
}
