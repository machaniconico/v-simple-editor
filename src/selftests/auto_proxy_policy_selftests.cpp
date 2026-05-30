// src/selftests/auto_proxy_policy_selftests.cpp
// Headless selftest for AutoProxyPolicy::decide().
// QApplication not required (pure Qt value-type engine).
// Pattern follows playback_quality_policy_selftests.cpp ([app] prefix, PASS/FAIL accumulation).

#include <cstdio>
#include "../playback/AutoProxyPolicy.h"

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace {

using playback::AutoProxyClip;
using playback::AutoProxyConfig;
using playback::AutoProxyPlan;
using playback::AutoProxyPolicy;

// Build a clip quickly.
AutoProxyClip makeClip(const char* path, int width, int height,
                       const char* codec, bool proxyReady)
{
    AutoProxyClip c;
    c.filePath   = QString::fromLatin1(path);
    c.width      = width;
    c.height     = height;
    c.codec      = QString::fromLatin1(codec);
    c.proxyReady = proxyReady;
    return c;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// runAutoProxyPolicySelftest
// Return 0 = all PASS, non-zero = number of failed gates.
// ---------------------------------------------------------------------------
int runAutoProxyPolicySelftest()
{
    int passed = 0;
    int failed = 0;

    // Default config reference values (not taken from the class; hand-computed):
    //   enabled=true, trackThreshold=2, heavyWidthThreshold=2560
    //   heavyCodecs = { "av1", "hevc", "h265", "vp9", "prores" }

    // --- Gate 1: enabled=false -> all lists empty, anyProxyActive=false ----
    {
        AutoProxyConfig cfg;
        cfg.enabled = false;

        QVector<AutoProxyClip> clips;
        // Add enough heavy clips that would normally trigger
        clips << makeClip("/a.mov", 3840, 2160, "hevc", true)
              << makeClip("/b.mov", 3840, 2160, "hevc", true);

        AutoProxyPlan plan = AutoProxyPolicy::decide(clips, cfg);
        bool ok = plan.useProxyFor.isEmpty() &&
                  plan.generateFor.isEmpty() &&
                  !plan.anyProxyActive;
        std::printf("[app] Gate  1 (enabled=false -> empty plan):                    %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 2: trackCount < threshold -> all lists empty -----------------
    {
        AutoProxyConfig cfg;
        cfg.trackThreshold = 3;  // require 3+

        // Only 2 clips (both heavy)
        QVector<AutoProxyClip> clips;
        clips << makeClip("/a.mov", 3840, 2160, "hevc", true)
              << makeClip("/b.mov", 3840, 2160, "hevc", true);

        AutoProxyPlan plan = AutoProxyPolicy::decide(clips, cfg);
        bool ok = plan.useProxyFor.isEmpty() &&
                  plan.generateFor.isEmpty() &&
                  !plan.anyProxyActive;
        std::printf("[app] Gate  2 (trackCount<threshold -> empty plan):             %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 3: heavy codec + proxyReady -> useProxyFor -------------------
    {
        QVector<AutoProxyClip> clips;
        clips << makeClip("/heavy_ready.mov", 1920, 1080, "hevc", true)
              << makeClip("/light.mp4",       1280, 720,  "h264", false);

        AutoProxyPlan plan = AutoProxyPolicy::decide(clips);  // default cfg
        bool ok = plan.useProxyFor.contains(QString("/heavy_ready.mov")) &&
                  !plan.useProxyFor.contains(QString("/light.mp4")) &&
                  !plan.generateFor.contains(QString("/heavy_ready.mov"));
        std::printf("[app] Gate  3 (heavy codec+ready -> useProxyFor):               %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 4: heavy codec + not ready -> generateFor --------------------
    {
        QVector<AutoProxyClip> clips;
        clips << makeClip("/need_proxy.mov", 1920, 1080, "av1", false)
              << makeClip("/light.mp4",      1280, 720,  "h264", false);

        AutoProxyPlan plan = AutoProxyPolicy::decide(clips);
        bool ok = plan.generateFor.contains(QString("/need_proxy.mov")) &&
                  !plan.useProxyFor.contains(QString("/need_proxy.mov")) &&
                  !plan.generateFor.contains(QString("/light.mp4"));
        std::printf("[app] Gate  4 (heavy codec+not ready -> generateFor):           %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 5: light clip (small res + h264) -> neither list -------------
    {
        QVector<AutoProxyClip> clips;
        clips << makeClip("/small_a.mp4", 1280, 720,  "h264", false)
              << makeClip("/small_b.mp4", 1920, 1080, "h264", false);

        AutoProxyPlan plan = AutoProxyPolicy::decide(clips);
        bool ok = plan.useProxyFor.isEmpty() &&
                  plan.generateFor.isEmpty() &&
                  !plan.anyProxyActive;
        std::printf("[app] Gate  5 (light clips -> neither list):                    %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 6: width-only heavy (>=2560) + proxyReady -> useProxyFor -----
    {
        QVector<AutoProxyClip> clips;
        // width=2560 (exactly at threshold), light codec, proxy ready
        clips << makeClip("/wide_ready.mov", 2560, 1440, "h264", true)
              << makeClip("/narrow.mp4",     1920, 1080, "h264", false);

        AutoProxyPlan plan = AutoProxyPolicy::decide(clips);
        bool ok = plan.useProxyFor.contains(QString("/wide_ready.mov")) &&
                  !plan.generateFor.contains(QString("/wide_ready.mov"));
        std::printf("[app] Gate  6 (width>=threshold+ready -> useProxyFor):          %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 7: codec comparison is case-insensitive ("AV1" hits) ---------
    {
        QVector<AutoProxyClip> clips;
        clips << makeClip("/upper_av1.mov", 1920, 1080, "AV1",   false)
              << makeClip("/mixed_hevc.mov", 1920, 1080, " HEVC ", false); // leading/trailing space

        AutoProxyPlan plan = AutoProxyPolicy::decide(clips);
        bool ok = plan.generateFor.contains(QString("/upper_av1.mov")) &&
                  plan.generateFor.contains(QString("/mixed_hevc.mov"));
        std::printf("[app] Gate  7 (case-insensitive codec match 'AV1'/'HEVC'):      %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 8: mixed sequence splits correctly ---------------------------
    {
        // 4 clips: heavy+ready, heavy+notready, light, heavy+notready(dup path)
        QVector<AutoProxyClip> clips;
        clips << makeClip("/hr.mov",  1920, 1080, "prores", true)   // -> useProxyFor
              << makeClip("/hn.mov",  1920, 1080, "vp9",    false)  // -> generateFor
              << makeClip("/lt.mp4",  1280, 720,  "h264",   false)  // -> neither
              << makeClip("/hr2.mov", 3840, 2160, "h264",   true);  // width-heavy+ready -> useProxyFor

        AutoProxyPlan plan = AutoProxyPolicy::decide(clips);
        bool ok = plan.useProxyFor.size() == 2 &&
                  plan.useProxyFor.contains(QString("/hr.mov")) &&
                  plan.useProxyFor.contains(QString("/hr2.mov")) &&
                  plan.generateFor.size() == 1 &&
                  plan.generateFor.contains(QString("/hn.mov")) &&
                  !plan.useProxyFor.contains(QString("/lt.mp4")) &&
                  !plan.generateFor.contains(QString("/lt.mp4"));
        std::printf("[app] Gate  8 (mixed sequence splits correctly):                %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 9: duplicate filePath deduplicated in each list --------------
    {
        QVector<AutoProxyClip> clips;
        // Same path appearing twice, both heavy+ready
        clips << makeClip("/dup.mov", 1920, 1080, "hevc", true)
              << makeClip("/dup.mov", 1920, 1080, "hevc", true)
              << makeClip("/other.mov", 1920, 1080, "av1", false); // need at least threshold=2

        AutoProxyPlan plan = AutoProxyPolicy::decide(clips);
        int dupCount = plan.useProxyFor.count(QString("/dup.mov"));
        bool ok = dupCount == 1;
        std::printf("[app] Gate  9 (duplicate filePath -> deduplicated to 1 entry):  %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 10: anyProxyActive accurate ----------------------------------
    {
        // Case A: useProxyFor non-empty -> anyProxyActive=true
        {
            QVector<AutoProxyClip> clipsA;
            clipsA << makeClip("/a.mov", 1920, 1080, "hevc", true)
                   << makeClip("/b.mov", 1920, 1080, "hevc", false);
            AutoProxyPlan pA = AutoProxyPolicy::decide(clipsA);
            bool okA = pA.anyProxyActive; // useProxyFor has /a.mov

            // Case B: only generateFor -> anyProxyActive=false
            QVector<AutoProxyClip> clipsB;
            clipsB << makeClip("/c.mov", 1920, 1080, "hevc", false)
                   << makeClip("/d.mov", 1920, 1080, "hevc", false);
            AutoProxyPlan pB = AutoProxyPolicy::decide(clipsB);
            bool okB = !pB.anyProxyActive; // only generateFor, no active proxy

            bool ok = okA && okB;
            std::printf("[app] Gate 10 (anyProxyActive accurate):                        %s\n", ok ? "PASS" : "FAIL");
            ok ? ++passed : ++failed;
        }
    }

    // --- Gate 11: empty clips list -> no crash, empty plan -----------------
    {
        QVector<AutoProxyClip> clips; // empty
        AutoProxyPlan plan = AutoProxyPolicy::decide(clips);
        bool ok = plan.useProxyFor.isEmpty() &&
                  plan.generateFor.isEmpty() &&
                  !plan.anyProxyActive;
        std::printf("[app] Gate 11 (empty clips -> no crash, empty plan):            %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 12: empty codec string -> width-only classification ----------
    {
        // clip with empty codec but wide enough -> still classified heavy via width
        QVector<AutoProxyClip> clips;
        clips << makeClip("/wide_empty_codec.mov", 3840, 2160, "", true)
              << makeClip("/narrow_empty.mp4",     1280, 720,  "", false);

        AutoProxyPlan plan = AutoProxyPolicy::decide(clips);
        bool ok = plan.useProxyFor.contains(QString("/wide_empty_codec.mov")) &&
                  !plan.generateFor.contains(QString("/wide_empty_codec.mov")) &&
                  !plan.useProxyFor.contains(QString("/narrow_empty.mp4")) &&
                  !plan.generateFor.contains(QString("/narrow_empty.mp4"));
        std::printf("[app] Gate 12 (empty codec -> width-only classification):       %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- summary ------------------------------------------------------------
    std::printf("[app] Result: %d/%d PASSED\n", passed, passed + failed);
    return failed; // 0 = all PASS
}
