#include "../ProjectFile.h"
#include "../Timeline.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

template <std::size_t N>
bool sameSamples(const std::array<int16_t, N> &a,
                 const std::array<int16_t, N> &b)
{
    for (std::size_t i = 0; i < N; ++i) {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

void pass(const char *gate, int &passed)
{
    ++passed;
    std::cout << "[audio-channel-map] PASS " << gate << '\n';
}

void fail(const char *gate, const std::string &message, int &failed)
{
    ++failed;
    std::cerr << "[audio-channel-map] FAIL " << gate << ": "
              << message << '\n';
}

ClipInfo makeClip()
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("channel-map.wav");
    clip.displayName = QStringLiteral("channel-map");
    clip.duration = 1.0;
    return clip;
}

} // namespace

int runAudioChannelMapSelftest()
{
    std::cout << "[audio-channel-map] selftest start\n";
    int passed = 0;
    int failed = 0;

    const std::array<int16_t, 8> stereo = {
        1000, -2000,
        3000, -4000,
        -5000, 6000,
        32767, -32768
    };

    {
        auto samples = stereo;
        const auto before = samples;
        applyAudioChannelModeToInterleavedStereoS16(
            samples.data(), static_cast<int>(samples.size() / 2),
            AudioChannelMode::Stereo);
        if (std::memcmp(samples.data(), before.data(),
                        sizeof(int16_t) * samples.size()) == 0) {
            pass("G1 Stereo is byte-identical", passed);
        } else {
            fail("G1 Stereo", "buffer changed in default mode", failed);
        }
    }

    {
        auto samples = stereo;
        const std::array<int16_t, 8> expected = {
            1000, 1000,
            3000, 3000,
            -5000, -5000,
            32767, 32767
        };
        applyAudioChannelModeToInterleavedStereoS16(
            samples.data(), static_cast<int>(samples.size() / 2),
            AudioChannelMode::FillLeft);
        if (sameSamples(samples, expected))
            pass("G2 FillLeft duplicates L into both channels", passed);
        else
            fail("G2 FillLeft", "sample placement mismatch", failed);
    }

    {
        auto samples = stereo;
        const std::array<int16_t, 8> expected = {
            -2000, -2000,
            -4000, -4000,
            6000, 6000,
            -32768, -32768
        };
        applyAudioChannelModeToInterleavedStereoS16(
            samples.data(), static_cast<int>(samples.size() / 2),
            AudioChannelMode::FillRight);
        if (sameSamples(samples, expected))
            pass("G3 FillRight duplicates R into both channels", passed);
        else
            fail("G3 FillRight", "sample placement mismatch", failed);
    }

    {
        auto samples = stereo;
        const std::array<int16_t, 8> expected = {
            -2000, 1000,
            -4000, 3000,
            6000, -5000,
            -32768, 32767
        };
        applyAudioChannelModeToInterleavedStereoS16(
            samples.data(), static_cast<int>(samples.size() / 2),
            AudioChannelMode::Swap);
        if (sameSamples(samples, expected))
            pass("G4 Swap exchanges L/R", passed);
        else
            fail("G4 Swap", "sample placement mismatch", failed);
    }

    {
        auto samples = stereo;
        const std::array<int16_t, 8> expected = {
            -500, -500,
            -500, -500,
            500, 500,
            0, 0
        };
        applyAudioChannelModeToInterleavedStereoS16(
            samples.data(), static_cast<int>(samples.size() / 2),
            AudioChannelMode::Mono);
        if (sameSamples(samples, expected))
            pass("G5 Mono averages L/R into both channels", passed);
        else
            fail("G5 Mono", "sample placement mismatch", failed);
    }

    {
        setAudioChannelModePlaybackBindings({
            {1500, 2, 4, AudioChannelMode::Swap}
        });
        PlaybackEntry entry;
        entry.clipIn = 1.5;
        entry.sourceTrack = 2;
        entry.sourceClipIndex = 4;
        PlaybackEntry missing = entry;
        missing.sourceClipIndex = 5;
        const bool ok = audioChannelModeForPlaybackEntry(entry) == AudioChannelMode::Swap
            && audioChannelModeForPlaybackEntry(missing) == AudioChannelMode::Stereo;
        setAudioChannelModePlaybackBindings({});
        if (ok)
            pass("G6 playback binding resolves mode and defaults missing to Stereo", passed);
        else
            fail("G6 playback binding", "mode lookup mismatch", failed);
    }

    {
        ProjectData data;
        QVector<ClipInfo> track;
        track.append(makeClip());
        data.audioTracks.append(track);

        const QString stereoJson = ProjectFile::toJsonString(data);
        const bool defaultOmitted =
            !stereoJson.contains(QStringLiteral("\"audioChannelMode\""));

        data.audioTracks[0][0].audioChannelMode = AudioChannelMode::FillLeft;
        const QString mappedJson = ProjectFile::toJsonString(data);
        ProjectData loaded;
        const bool loadedOk = ProjectFile::fromJsonString(mappedJson, loaded);
        const bool hasLoadedClip = loadedOk
            && !loaded.audioTracks.isEmpty()
            && !loaded.audioTracks[0].isEmpty();
        const bool roundTrips = mappedJson.contains(QStringLiteral("\"audioChannelMode\""))
            && hasLoadedClip
            && loaded.audioTracks[0][0].audioChannelMode == AudioChannelMode::FillLeft;

        if (defaultOmitted && roundTrips)
            pass("G7 ProjectFile default omit and FillLeft round-trip", passed);
        else
            fail("G7 ProjectFile", "default omission or mapped round-trip failed", failed);
    }

    {
        struct ExpectedPan {
            AudioChannelMode mode;
            const char *pan;
        };
        const std::array<ExpectedPan, 5> expected = {{
            {AudioChannelMode::Stereo, ""},
            {AudioChannelMode::FillLeft, "pan=stereo|c0=c0|c1=c0"},
            {AudioChannelMode::FillRight, "pan=stereo|c0=c1|c1=c1"},
            {AudioChannelMode::Swap, "pan=stereo|c0=c1|c1=c0"},
            {AudioChannelMode::Mono, "pan=stereo|c0=0.5*c0+0.5*c1|c1=0.5*c0+0.5*c1"}
        }};

        bool ok = true;
        std::string message;
        for (const ExpectedPan &item : expected) {
            const QString expectedPan = QString::fromLatin1(item.pan);
            const QString actualPan = exportAudioChannelPanFilterForMode(item.mode);
            if (actualPan != expectedPan) {
                ok = false;
                message = "pan expression mismatch";
                break;
            }

            const QString chain = buildExportAudioMixEntryFilterChain(
                0,
                QStringLiteral("0"),
                QStringLiteral("1"),
                0,
                QStringLiteral("1"),
                item.mode);
            const int panPos = chain.indexOf(QStringLiteral("pan=stereo"));
            const int volumePos = chain.indexOf(QStringLiteral("volume="));
            if (expectedPan.isEmpty()) {
                if (panPos >= 0) {
                    ok = false;
                    message = "Stereo export chain unexpectedly contains pan";
                    break;
                }
            } else if (!chain.contains(expectedPan)
                       || panPos < 0
                       || volumePos < 0
                       || panPos > volumePos) {
                ok = false;
                message = "export chain missing pan before volume";
                break;
            }
        }

        if (ok)
            pass("G8 export filter chain maps each mode before volume", passed);
        else
            fail("G8 export filter chain", message, failed);
    }

    std::cout << "[audio-channel-map] summary passed=" << passed
              << " failed=" << failed << '\n';
    return failed == 0 ? 0 : 1;
}
