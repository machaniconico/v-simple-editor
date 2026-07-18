#include <cstdio>
#include <cmath>

#include "../playback/PlaybackQualityPolicy.h"

// PlaybackQualityPolicy 単体テスト。
// QApplication 不要 (純粋 C++ エンジン、Qt 依存ゼロ)。
// 流儀は auto_matte_selftest.cpp に倣う ([pbq] プレフィックス, PASS/FAIL 集計)。

namespace {

// ヘルパー: PlaybackMetrics を簡単に作る
playback::PlaybackMetrics makeMetrics(
    int    trackCount,
    double lastFrameRenderMs,
    double targetFrameMs,
    bool   isPlaying      = true,
    bool   proxyAvailable = false,
    int    activeClipCount = 1)
{
    playback::PlaybackMetrics m;
    m.trackCount         = trackCount;
    m.activeClipCount    = activeClipCount;
    m.lastFrameRenderMs  = lastFrameRenderMs;
    m.targetFrameMs      = targetFrameMs;
    m.isPlaying          = isPlaying;
    m.proxyAvailable     = proxyAvailable;
    return m;
}

// 浮動小数点比較
bool nearEqual(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) < eps;
}

} // anonymous namespace

int runPlaybackQualityPolicySelftest()
{
    int passed = 0;
    int failed = 0;

    // デフォルト設定:
    //   slowFactor=1.2, recoverFactor=0.7, proxyTrackThreshold=3, minScale=0.25
    // targetFrameMs=16.67 (60fps) を基準に:
    //   slow    = 16.67 * 1.2 = 20.00 ms
    //   recover = 16.67 * 0.7 = 11.67 ms
    const double TARGET = 16.67;

    // --- Gate 1: isPlaying=false -> {1.0, false, false} ------------------
    {
        playback::PlaybackQualityPolicy p;
        // まず劣化させる
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET)); // slow -> 0.5
        // 停止
        auto d = p.decide(makeMetrics(1, TARGET * 1.5, TARGET, /*isPlaying=*/false));
        bool ok = nearEqual(d.scaleFactor, 1.0) && !d.useProxy && !d.allowFrameDrop;
        std::printf("[pbq] Gate 1 (stopped -> full quality): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 2: 速いrender でフル品質維持 (scale=1.0 のまま) -------------
    {
        playback::PlaybackQualityPolicy p;
        // render が recover を大きく下回る (fast)
        auto d = p.decide(makeMetrics(1, TARGET * 0.5, TARGET));
        bool ok = nearEqual(d.scaleFactor, 1.0);
        std::printf("[pbq] Gate 2 (fast render -> scale stays 1.0): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 3: 遅いrender で1段劣化 (1.0 -> 0.5) -----------------------
    {
        playback::PlaybackQualityPolicy p;
        auto d = p.decide(makeMetrics(1, TARGET * 1.5, TARGET)); // slow
        bool ok = nearEqual(d.scaleFactor, 0.5);
        std::printf("[pbq] Gate 3 (slow render -> scale=0.5): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 4: さらに遅いと最小scale へ (0.5 -> minScale=0.25) ---------
    {
        playback::PlaybackQualityPolicy p;
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET)); // 1.0 -> 0.5
        auto d = p.decide(makeMetrics(1, TARGET * 1.5, TARGET)); // 0.5 -> 0.25
        bool ok = nearEqual(d.scaleFactor, 0.25);
        std::printf("[pbq] Gate 4 (two slow ticks -> minScale=0.25): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 5: proxyAvailable=false -> useProxy 常に false -------------
    {
        playback::PlaybackQualityPolicy p;
        // 遅くて trackCount もしきい値以上、だが proxy なし
        auto d = p.decide(makeMetrics(5, TARGET * 1.5, TARGET, true, /*proxyAvailable=*/false));
        bool ok = !d.useProxy;
        std::printf("[pbq] Gate 5 (no proxy available -> useProxy=false): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 6: trackCount>=threshold で useProxy=true (proxy あり) -----
    {
        playback::PlaybackQualityPolicy p;
        // trackCount=3 (threshold=3), proxyAvailable=true, render は fast でも OK
        // scaleFactor=1.0 でも trackCount>=threshold なら useProxy=true
        auto d = p.decide(makeMetrics(3, TARGET * 0.5, TARGET, true, /*proxyAvailable=*/true));
        bool ok = d.useProxy;
        std::printf("[pbq] Gate 6 (trackCount>=threshold+proxy -> useProxy=true): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 7: ヒステリシス — 境界付近を交互に与えても振動しない --------
    // recover < render < slow のバンド内では scaleFactor を変えない。
    // 「少し遅い」と「少し速い」のどちらでも振動しないことを10tick確認。
    {
        playback::PlaybackQualityPolicy p;
        // まず1段劣化 (scale=0.5) にする
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET));

        // ヒステリシスバンド内: recover(0.7) < render/target < slow(1.2)
        // render = TARGET * 1.0 (バンド中央付近) を何度与えても 0.5 のまま
        double renderInBand = TARGET * 1.0;
        bool oscillated = false;
        double prevScale = 0.5;
        for (int i = 0; i < 10; ++i) {
            auto d = p.decide(makeMetrics(1, renderInBand, TARGET));
            if (!nearEqual(d.scaleFactor, prevScale)) {
                oscillated = true;
                break;
            }
        }
        bool ok = !oscillated;
        std::printf("[pbq] Gate 7 (hysteresis band - no oscillation): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 8: 同一入力列で決定的に同じ出力 ----------------------------
    {
        // 2つの独立インスタンスに同じ入力シーケンスを与えて出力が一致するか
        playback::PlaybackQualityPolicy p1;
        playback::PlaybackQualityPolicy p2;

        struct Tick { double renderMs; };
        Tick ticks[] = {
            { TARGET * 1.5 }, // slow
            { TARGET * 1.5 }, // slow
            { TARGET * 0.3 }, // fast (recover)
            { TARGET * 1.0 }, // band (hold)
            { TARGET * 1.5 }, // slow
        };

        bool allMatch = true;
        for (auto& t : ticks) {
            auto d1 = p1.decide(makeMetrics(2, t.renderMs, TARGET));
            auto d2 = p2.decide(makeMetrics(2, t.renderMs, TARGET));
            if (!nearEqual(d1.scaleFactor, d2.scaleFactor) ||
                d1.useProxy != d2.useProxy ||
                d1.allowFrameDrop != d2.allowFrameDrop) {
                allMatch = false;
                break;
            }
        }
        std::printf("[pbq] Gate 8 (deterministic - same input same output): %s\n", allMatch ? "PASS" : "FAIL");
        allMatch ? ++passed : ++failed;
    }

    // --- Gate 9: scaleFactor が minScale でクランプ -----------------------
    {
        playback::PlaybackQualityPolicy p;
        // 何度遅くしても minScale=0.25 より下がらない
        for (int i = 0; i < 10; ++i) {
            p.decide(makeMetrics(1, TARGET * 2.0, TARGET));
        }
        auto d = p.decide(makeMetrics(1, TARGET * 2.0, TARGET));
        bool ok = nearEqual(d.scaleFactor, 0.25) && d.scaleFactor >= 0.25;
        std::printf("[pbq] Gate 9 (scaleFactor clamped at minScale=0.25): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 10: reset() 後はフル品質から再開 ----------------------------
    {
        playback::PlaybackQualityPolicy p;
        // 劣化させる
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET)); // 0.5
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET)); // 0.25
        // reset
        p.reset();
        // 次の tick が hysteresis band 内なら 1.0 を維持する
        auto d = p.decide(makeMetrics(1, TARGET * 1.0, TARGET)); // band -> hold at 1.0
        bool ok = nearEqual(d.scaleFactor, 1.0);
        std::printf("[pbq] Gate 10 (after reset -> full quality restart): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 11: allowFrameDrop は minScale + 遅い時のみ true ------------
    {
        playback::PlaybackQualityPolicy p;
        // minScale まで劣化
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET));
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET));
        // まだ遅い -> allowFrameDrop=true
        auto d = p.decide(makeMetrics(1, TARGET * 1.5, TARGET));
        bool ok = nearEqual(d.scaleFactor, 0.25) && d.allowFrameDrop;
        std::printf("[pbq] Gate 11 (minScale+slow -> allowFrameDrop=true): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 12: allowFrameDrop=false when render recovers ---------------
    {
        playback::PlaybackQualityPolicy p;
        // minScale まで劣化してから render が fast になった場合
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET));
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET));
        // 速くなった -> allowFrameDrop は false (slow 条件不成立)
        auto d = p.decide(makeMetrics(1, TARGET * 0.3, TARGET)); // fast -> stepUp to 0.5
        bool ok = !d.allowFrameDrop;
        std::printf("[pbq] Gate 12 (fast render -> allowFrameDrop=false): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 13: scaleFactor degraded -> useProxy=true even trackCount<threshold --
    {
        playback::PlaybackQualityPolicy p;
        // trackCount=1 (< threshold=3), だが scale が落ちていれば useProxy=true
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET)); // scale=0.5
        auto d = p.decide(makeMetrics(1, TARGET * 1.0, TARGET, true, /*proxy=*/true)); // band: hold 0.5
        bool ok = d.useProxy; // scaleFactor < 1.0 なので proxy 推奨
        std::printf("[pbq] Gate 13 (degraded scale+proxy -> useProxy=true even trackCount<threshold): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 14: recover シーケンス: minScale -> 0.5 -> 1.0 (2 fast tick) --
    {
        playback::PlaybackQualityPolicy p;
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET)); // 1.0 -> 0.5
        p.decide(makeMetrics(1, TARGET * 1.5, TARGET)); // 0.5 -> 0.25
        // 2回 fast で 0.25 -> 0.5 -> 1.0
        auto d1 = p.decide(makeMetrics(1, TARGET * 0.3, TARGET)); // 0.25 -> 0.5
        auto d2 = p.decide(makeMetrics(1, TARGET * 0.3, TARGET)); // 0.5  -> 1.0
        bool ok = nearEqual(d1.scaleFactor, 0.5) && nearEqual(d2.scaleFactor, 1.0);
        std::printf("[pbq] Gate 14 (two fast ticks: minScale->0.5->1.0): %s\n", ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // 結果サマリ
    std::printf("[pbq] Result: %d/%d PASSED\n", passed, passed + failed);
    return failed; // 0 = 全 PASS
}
