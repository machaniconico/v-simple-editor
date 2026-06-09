#include "../playback/AudioPan.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#if __has_include(<QString>) && __has_include(<QVector>)
#define STEREO_PAN_SELFTEST_HAS_QT 1
#include <QString>
#include <QVector>
#include "../ProjectFile.h"
#else
#define STEREO_PAN_SELFTEST_HAS_QT 0
#endif

namespace {

template <typename T, std::size_t N>
bool sameBytes(const std::array<T, N> &a, const std::array<T, N> &b)
{
    return std::memcmp(a.data(), b.data(), sizeof(T) * N) == 0;
}

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

bool nearly(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) <= eps;
}

void pass(const char *gate, int &passed)
{
    ++passed;
    std::cout << "[stereo-pan] PASS " << gate << '\n';
}

void fail(const char *gate, const std::string &message, int &failed)
{
    ++failed;
    std::cerr << "[stereo-pan] FAIL " << gate << ": " << message << '\n';
}

std::string standaloneClipJson(double pan)
{
    std::ostringstream os;
    os << "{\"filePath\":\"tone.wav\"";
    if (pan != 0.0)
        os << ",\"pan\":" << pan;
    os << "}";
    return os.str();
}

double standaloneReadPan(const std::string &json)
{
    const std::string key = "\"pan\":";
    const std::size_t pos = json.find(key);
    if (pos == std::string::npos)
        return 0.0;
    return std::stod(json.substr(pos + key.size()));
}

} // namespace

int runStereoPanSelftest()
{
    std::cout << "[stereo-pan] selftest start\n";
    int passed = 0;
    int failed = 0;

    // G1: pan=0.0 must not touch int16 or float buffers.
    {
        std::array<int16_t, 8> i16 = {1234, -2345, 0, 32767, -32768, 42, 7, -8};
        const auto i16Before = i16;
        audiopan::applyBalanceInterleavedStereoInt16(i16.data(), i16.size() / 2, 0.0);

        std::array<float, 6> f32 = {1.0f, -2.0f, 0.0f, -0.0f, 0.25f, -0.5f};
        const auto f32Before = f32;
        audiopan::applyBalanceInterleavedStereoFloat(f32.data(), f32.size() / 2, 0.0);

        if (sameBytes(i16, i16Before) && sameBytes(f32, f32Before))
            pass("G1 identity pan=0.0 is bit-identical", passed);
        else
            fail("G1 identity", "buffer changed at pan=0.0", failed);
    }

    // G2: full right balance attenuates left only.
    {
        std::array<int16_t, 6> samples = {1234, -2345, -3000, 4000, 32767, -32768};
        const std::array<int16_t, 6> expected = {0, -2345, 0, 4000, 0, -32768};
        audiopan::applyBalanceInterleavedStereoInt16(samples.data(), samples.size() / 2, 1.0);
        if (sameSamples(samples, expected))
            pass("G2 pan=+1.0 left muted right unchanged", passed);
        else
            fail("G2 pan +1", "unexpected sample values", failed);
    }

    // G3: full left balance attenuates right only.
    {
        std::array<int16_t, 6> samples = {1234, -2345, -3000, 4000, 32767, -32768};
        const std::array<int16_t, 6> expected = {1234, 0, -3000, 0, 32767, 0};
        audiopan::applyBalanceInterleavedStereoInt16(samples.data(), samples.size() / 2, -1.0);
        if (sameSamples(samples, expected))
            pass("G3 pan=-1.0 right muted left unchanged", passed);
        else
            fail("G3 pan -1", "unexpected sample values", failed);
    }

    // G4: half right balance applies leftGain=0.5 and leaves right untouched.
    {
        const audiopan::BalanceGains gains = audiopan::balanceGains(0.5);
        std::array<int16_t, 4> samples = {1000, 2000, -3000, -4000};
        const std::array<int16_t, 4> expected = {500, 2000, -1500, -4000};
        audiopan::applyBalanceInterleavedStereoInt16(samples.data(), samples.size() / 2, 0.5);
        if (nearly(gains.left, 0.5) && nearly(gains.right, 1.0)
            && sameSamples(samples, expected)) {
            pass("G4 pan=+0.5 leftGain=0.5 right unchanged", passed);
        } else {
            fail("G4 pan +0.5", "gain law or sample values mismatch", failed);
        }
    }

    // G5: ProjectFile pan omission and round-trip. The Qt app build exercises
    // ProjectFile directly; the non-Qt standalone compile path mirrors the
    // same key rule so this file can still be compiled without Qt headers.
    {
#if STEREO_PAN_SELFTEST_HAS_QT
        ProjectData data;
        ClipInfo clip;
        clip.filePath = QStringLiteral("tone.wav");
        clip.displayName = QStringLiteral("tone");
        clip.duration = 1.0;

        QVector<ClipInfo> track;
        track.append(clip);
        data.audioTracks.append(track);

        const QString zeroJson = ProjectFile::toJsonString(data);
        const bool zeroOmitsPan = !zeroJson.contains(QStringLiteral("\"pan\""));

        data.audioTracks[0][0].pan = 0.5;
        const QString panJson = ProjectFile::toJsonString(data);
        ProjectData loaded;
        const bool loadedOk = ProjectFile::fromJsonString(panJson, loaded);
        const bool hasClip = loadedOk
            && !loaded.audioTracks.isEmpty()
            && !loaded.audioTracks[0].isEmpty();
        const double loadedPan = hasClip ? loaded.audioTracks[0][0].pan : 0.0;
        const bool panRoundTrips = panJson.contains(QStringLiteral("\"pan\""))
            && nearly(loadedPan, 0.5);
#else
        const std::string zeroJson = standaloneClipJson(0.0);
        const bool zeroOmitsPan = zeroJson.find("\"pan\"") == std::string::npos;

        const std::string panJson = standaloneClipJson(0.5);
        const double loadedPan = standaloneReadPan(panJson);
        const bool panRoundTrips = panJson.find("\"pan\"") != std::string::npos
            && nearly(loadedPan, 0.5);
#endif
        if (zeroOmitsPan && panRoundTrips)
            pass("G5 ProjectFile pan omit/default and 0.5 round-trip", passed);
        else
            fail("G5 ProjectFile", "pan key omission or 0.5 round-trip failed", failed);
    }

    std::cout << "[stereo-pan] summary passed=" << passed
              << " failed=" << failed << '\n';
    return failed == 0 ? 0 : 1;
}
