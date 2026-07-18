// live_matte_resolve_selftests.cpp
// Headless selftest for the pure clipstack::resolveLiveMatteSources resolver
// (src/playback/LiveMatteResolve.{h,cpp}), the STAGE4B live track-matte index
// resolver that mirrors the export path (TimelineFrameRenderer.cpp:792-814).
// No QApplication, no GUI — std::function + QHash<QString,int> only.
// Run via: --selftest=live-matte-resolve
//
// Verification strategy: every gate uses a HAND-COMPUTED expected srcIdx, never
// the resolver's own output, so a regression cannot pass tautologically. The
// callbacks read from a small in-memory layer table built per gate.
//
// Gate map (8 gates):
//   G1 valid: 3-layer set, layer idx2 mattes off clipId of idx1 -> srcIdx 1
//   G2 reject self-reference: layer idx2 mattes off its OWN clipId -> -1
//   G3 reject base: layer idx2 mattes off clipId of idx0 (V1 base) -> -1
//   G4 reject unknown: layer idx2 mattes off a clipId not in the set -> -1
//   G5 no-matte layer stays -1 (hasMatte=false even with a srcClipId present)
//   G6 multi-matte: idx1 mattes off idx2 (valid) AND idx2 mattes off idx0
//      (rejected base) -> idx1=2, idx2=-1 (resolver is order-independent)
//   --- index-space CONVENTION gates (pin the live caller's ordering, C1) ---
//   G7 V1-FIRST order (what tryGpuComposeLayers feeds for matte scenes after the
//      C1 fix): [V1@0, V2@1 mattes off V3, V3@2] -> idx1 resolves to 2. This is
//      the COMMON "overlay matted by a higher overlay" topology and MUST resolve.
//   G8 V1-LAST order (the raw paintOrder-sorted vector, V1 at the end): the SAME
//      logical scene [V3@0, V2@1 mattes off V3="2:0", V1@2] makes the matte source
//      land at index 0 -> WRONGLY rejected (-1). Documents why the live caller
//      MUST reorder inputs V1-first before resolving (else common mattes vanish).

#include <cstdio>

#include <QString>
#include <QVector>

#include "../playback/LiveMatteResolve.h"

namespace {

// Minimal per-layer record driving the resolver callbacks.
struct Lyr {
    QString clipId;       // this layer's clipId ("trackIdx:clipIdx")
    QString matteSrcId;   // matteSourceClipId ("" == none)
    bool hasMatte = false;
};

// Run the resolver over a layer table; return the resolved srcIdx vector
// (initialised to -1, the caller's default == "composite normally").
QVector<int> resolve(const QVector<Lyr>& layers)
{
    const int n = layers.size();
    QVector<int> out(n, -1);
    clipstack::resolveLiveMatteSources(
        n,
        [&](int i) { return layers[i].clipId; },
        [&](int i) { return layers[i].matteSrcId; },
        [&](int i) { return layers[i].hasMatte; },
        [&](int idx, int srcIdx) { out[idx] = srcIdx; });
    return out;
}

} // anonymous namespace

int runLiveMatteResolveSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[live-matte-resolve] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    // G1: valid. idx2 mattes off idx1's clipId -> resolves to 1.
    {
        QVector<Lyr> layers = {
            { "0:0", "",    false },  // idx0 base
            { "1:0", "",    false },  // idx1 (matte source)
            { "2:0", "1:0", true  },  // idx2 mattes off idx1
        };
        const QVector<int> r = resolve(layers);
        check(1, "idx2 mattes off idx1 -> srcIdx 1",
              r.size() == 3 && r[0] == -1 && r[1] == -1 && r[2] == 1);
    }

    // G2: reject self-reference. idx2 mattes off its OWN clipId -> -1.
    {
        QVector<Lyr> layers = {
            { "0:0", "",    false },
            { "1:0", "",    false },
            { "2:0", "2:0", true  },  // self-reference
        };
        const QVector<int> r = resolve(layers);
        check(2, "self-reference rejected -> -1", r[2] == -1);
    }

    // G3: reject base. idx2 mattes off idx0 (V1 base, index 0) -> -1.
    {
        QVector<Lyr> layers = {
            { "0:0", "",    false },
            { "1:0", "",    false },
            { "2:0", "0:0", true  },  // resolves to index 0 -> rejected
        };
        const QVector<int> r = resolve(layers);
        check(3, "base (index 0) rejected -> -1", r[2] == -1);
    }

    // G4: reject unknown. idx2 mattes off a clipId not present -> -1.
    {
        QVector<Lyr> layers = {
            { "0:0", "",      false },
            { "1:0", "",      false },
            { "2:0", "9:9",   true  },  // unknown clipId
        };
        const QVector<int> r = resolve(layers);
        check(4, "unknown clipId rejected -> -1", r[2] == -1);
    }

    // G5: no-matte layer stays -1 even if a srcClipId string is present
    // (hasMatte=false means the layer is never resolved).
    {
        QVector<Lyr> layers = {
            { "0:0", "",    false },
            { "1:0", "",    false },
            { "2:0", "1:0", false },  // valid src but hasMatte=false
        };
        const QVector<int> r = resolve(layers);
        check(5, "no-matte layer stays -1", r[2] == -1);
    }

    // G6: multi-matte, order-independent. idx1 mattes off idx2 (valid -> 2),
    // idx2 mattes off idx0 base (rejected -> -1).
    {
        QVector<Lyr> layers = {
            { "0:0", "",    false },  // idx0 base
            { "1:0", "2:0", true  },  // idx1 mattes off idx2 -> 2
            { "2:0", "0:0", true  },  // idx2 mattes off base -> rejected
        };
        const QVector<int> r = resolve(layers);
        check(6, "multi-matte: idx1->2 valid, idx2->base rejected",
              r[0] == -1 && r[1] == 2 && r[2] == -1);
    }

    // G7: index-space convention — V1-FIRST order (what the live caller feeds for
    // matte scenes after the C1 fix: inputs sorted ascending by sourceTrack). The
    // common topology "V2 matted by a higher overlay V3" must resolve: idx1 (V2)
    // mattes off idx2 (V3) -> srcIdx 2 (>0, valid).
    {
        QVector<Lyr> layers = {
            { "0:0", "",    false },  // idx0 = V1 base (track 0)
            { "1:0", "2:0", true  },  // idx1 = V2, mattes off V3
            { "2:0", "",    false },  // idx2 = V3 (matte source, higher track)
        };
        const QVector<int> r = resolve(layers);
        check(7, "V1-FIRST: V2 matted by higher overlay V3 -> srcIdx 2",
              r.size() == 3 && r[0] == -1 && r[1] == 2 && r[2] == -1);
    }

    // G8: same logical scene fed in V1-LAST order (the raw paintOrder-sorted
    // vector, highest track at index 0). V3 (the matte source) now sits at index 0
    // -> the srcIdx>0 rule WRONGLY rejects it (-1). This is the C1 failure the
    // live caller avoids by reordering V1-first; the gate locks that contract in.
    {
        QVector<Lyr> layers = {
            { "2:0", "",    false },  // idx0 = V3 (highest track) — the matte source
            { "1:0", "2:0", true  },  // idx1 = V2, mattes off V3 ("2:0") -> index 0
            { "0:0", "",    false },  // idx2 = V1 base (track 0)
        };
        const QVector<int> r = resolve(layers);
        check(8, "V1-LAST: matte source at index 0 wrongly rejected (proves C1)",
              r.size() == 3 && r[1] == -1);
    }

    std::printf("[live-matte-resolve] Result: %d/%d PASSED\n",
                passed, passed + failed);
    return failed; // 0 = all PASS
}
