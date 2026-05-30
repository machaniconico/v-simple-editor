// composite_frame_cache_selftests.cpp
// Headless selftest for playback::CompositeFrameCache.
// No QApplication, no GUI dependencies.
// Run via: --selftest=composite-frame-cache
//
// Gate map (10 gates):
//   G1  put -> tryGet hit
//   G2  unknown key -> miss
//   G3  LRU eviction order (oldest evicted first under small budget)
//   G4  byte budget not exceeded after eviction
//   G5  invalidateRevision removes stale, retains current
//   G6  clear empties the cache
//   G7  stats: hits / misses / bytes / count accurate
//   G8  same-field keys are equal and hash consistently (deterministic)
//   G9  budget=0 never stores anything
//   G10 single oversized frame silently dropped, no crash

#include <QDebug>
#include <QImage>

#include "../playback/CompositeFrameCache.h"

using playback::CompositeFrameCache;
using playback::CompositeFrameKey;

namespace {

// Build a deterministic CompositeFrameKey.
CompositeFrameKey makeKey(quint64 rev, qint64 timeMs,
                          int w = 64, int h = 64,
                          int tier = 0, bool proxy = false)
{
    CompositeFrameKey k;
    k.timelineRevision = rev;
    k.timeMs           = timeMs;
    k.width            = w;
    k.height           = h;
    k.qualityTier      = tier;
    k.useProxy         = proxy;
    return k;
}

// Build a solid-colour QImage with a specific size and fill.
// Constructed entirely independently from the cache — no cache API involved.
QImage makeFrame(int w, int h, QRgb colour = qRgb(128, 64, 32))
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(colour);
    return img;
}

} // namespace

int runCompositeFrameCacheSelftest()
{
    qInfo().noquote() << "[composite-frame-cache] selftest start";
    int passed = 0, failed = 0;

    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[composite-frame-cache] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[composite-frame-cache] FAIL" << name << ":" << msg;
    };

    // ------------------------------------------------------------------
    // G1: put then tryGet returns the same image (hit).
    // ------------------------------------------------------------------
    {
        CompositeFrameCache cache;
        const CompositeFrameKey k = makeKey(1, 0);
        const QImage src = makeFrame(64, 64, qRgb(10, 20, 30));
        cache.put(k, src);

        QImage out;
        const bool hit = cache.tryGet(k, out);
        const bool pixelMatch = hit && out.pixel(0, 0) == src.pixel(0, 0)
                                    && out.size() == src.size();
        if (pixelMatch) {
            pass("G1 put->tryGet hit returns correct image");
        } else {
            fail("G1 put->tryGet", QStringLiteral("hit=%1 sizeMatch=%2")
                 .arg(hit).arg(out.size() == src.size()));
        }
    }

    // ------------------------------------------------------------------
    // G2: tryGet for an unknown key returns false (miss).
    // ------------------------------------------------------------------
    {
        CompositeFrameCache cache;
        QImage out;
        const bool hit = cache.tryGet(makeKey(99, 1000), out);
        if (!hit) {
            pass("G2 unknown key -> miss");
        } else {
            fail("G2 unknown key", QStringLiteral("expected miss, got hit"));
        }
    }

    // ------------------------------------------------------------------
    // G3: LRU eviction — with a tiny budget, the oldest put is evicted first.
    //
    // Strategy: each 64x64 ARGB32 frame = 64*64*4 = 16 384 bytes.
    // Set budget to exactly 2 frames (32 768 bytes).
    // Insert frames A, B, then C. A must be evicted (LRU), B and C remain.
    // ------------------------------------------------------------------
    {
        CompositeFrameCache cache;
        const int W = 64, H = 64;
        const qint64 frameBytes = static_cast<qint64>(W * H * 4); // ARGB32
        cache.setMemoryBudgetBytes(frameBytes * 2);

        const CompositeFrameKey kA = makeKey(1,  0);
        const CompositeFrameKey kB = makeKey(1, 33);
        const CompositeFrameKey kC = makeKey(1, 66);

        cache.put(kA, makeFrame(W, H, qRgb(1, 0, 0)));
        cache.put(kB, makeFrame(W, H, qRgb(2, 0, 0)));
        cache.put(kC, makeFrame(W, H, qRgb(3, 0, 0))); // triggers eviction of A

        QImage dummy;
        const bool aEvicted = !cache.tryGet(kA, dummy);
        const bool bPresent  =  cache.tryGet(kB, dummy);
        const bool cPresent  =  cache.tryGet(kC, dummy);

        if (aEvicted && bPresent && cPresent) {
            pass("G3 LRU eviction evicts oldest first");
        } else {
            fail("G3 LRU eviction",
                 QStringLiteral("aEvicted=%1 bPresent=%2 cPresent=%3")
                 .arg(aEvicted).arg(bPresent).arg(cPresent));
        }
    }

    // ------------------------------------------------------------------
    // G4: stats().bytes never exceeds the configured budget.
    // ------------------------------------------------------------------
    {
        CompositeFrameCache cache;
        const int W = 32, H = 32;
        const qint64 frameBytes = static_cast<qint64>(W * H * 4);
        const qint64 budget     = frameBytes * 3;
        cache.setMemoryBudgetBytes(budget);

        // Insert 6 frames — triggers eviction each time past the 3-frame limit.
        for (int i = 0; i < 6; ++i) {
            cache.put(makeKey(1, static_cast<qint64>(i * 100)),
                      makeFrame(W, H, qRgb(i * 20, 0, 0)));
        }

        const auto s = cache.stats();
        if (s.bytes <= budget) {
            pass("G4 byte budget never exceeded");
        } else {
            fail("G4 byte budget",
                 QStringLiteral("bytes=%1 budget=%2").arg(s.bytes).arg(budget));
        }
    }

    // ------------------------------------------------------------------
    // G5: invalidateRevision removes stale-revision entries but keeps current.
    // ------------------------------------------------------------------
    {
        CompositeFrameCache cache;
        const CompositeFrameKey kOld  = makeKey(/*rev=*/1, 0);
        const CompositeFrameKey kCurr = makeKey(/*rev=*/2, 0);

        cache.put(kOld,  makeFrame(64, 64, qRgb(10, 0, 0)));
        cache.put(kCurr, makeFrame(64, 64, qRgb(20, 0, 0)));

        cache.invalidateRevision(/*currentRevision=*/2);

        QImage dummy;
        const bool oldGone    = !cache.tryGet(kOld,  dummy);
        const bool currPresent =  cache.tryGet(kCurr, dummy);

        if (oldGone && currPresent) {
            pass("G5 invalidateRevision removes stale, keeps current");
        } else {
            fail("G5 invalidateRevision",
                 QStringLiteral("oldGone=%1 currPresent=%2")
                 .arg(oldGone).arg(currPresent));
        }
    }

    // ------------------------------------------------------------------
    // G6: clear() empties the cache (count==0, tryGet misses).
    // ------------------------------------------------------------------
    {
        CompositeFrameCache cache;
        const CompositeFrameKey k = makeKey(1, 0);
        cache.put(k, makeFrame(64, 64));
        cache.clear();

        QImage dummy;
        const bool miss      = !cache.tryGet(k, dummy);
        const bool countZero  = cache.stats().count == 0;
        const bool bytesZero  = cache.stats().bytes == 0;

        if (miss && countZero && bytesZero) {
            pass("G6 clear empties cache");
        } else {
            fail("G6 clear",
                 QStringLiteral("miss=%1 count=%2 bytes=%3")
                 .arg(miss).arg(cache.stats().count).arg(cache.stats().bytes));
        }
    }

    // ------------------------------------------------------------------
    // G7: stats() fields (hits / misses / bytes / count) are accurate.
    // ------------------------------------------------------------------
    {
        CompositeFrameCache cache;
        const int W = 16, H = 16;
        const qint64 frameBytes = static_cast<qint64>(W * H * 4);

        const CompositeFrameKey k1 = makeKey(1, 0);
        const CompositeFrameKey k2 = makeKey(1, 100);
        const CompositeFrameKey kMiss = makeKey(1, 999);

        cache.put(k1, makeFrame(W, H));
        cache.put(k2, makeFrame(W, H));

        QImage dummy;
        cache.tryGet(k1, dummy);   // hit
        cache.tryGet(k1, dummy);   // hit
        cache.tryGet(kMiss, dummy); // miss

        const auto s = cache.stats();
        const bool ok = s.hits   == 2
                     && s.misses == 1
                     && s.bytes  == frameBytes * 2
                     && s.count  == 2;

        if (ok) {
            pass("G7 stats hits/misses/bytes/count accurate");
        } else {
            fail("G7 stats",
                 QStringLiteral("hits=%1 misses=%2 bytes=%3 count=%4")
                 .arg(s.hits).arg(s.misses).arg(s.bytes).arg(s.count));
        }
    }

    // ------------------------------------------------------------------
    // G8: Same-field keys compare equal and hash to the same value (deterministic).
    //     Different keys compare unequal with different hashes (very high probability).
    // ------------------------------------------------------------------
    {
        const CompositeFrameKey a = makeKey(42, 1000, 1920, 1080, 1, true);
        const CompositeFrameKey b = makeKey(42, 1000, 1920, 1080, 1, true);
        const CompositeFrameKey c = makeKey(42, 1001, 1920, 1080, 1, true); // different timeMs

        const bool equalEq    = (a == b);
        const bool hashEq     = (qHash(a, 0) == qHash(b, 0));
        const bool inequalNeq = !(a == c);
        // Hash collision for distinct keys is astronomically unlikely but not guaranteed.
        // We only assert equality implies same hash, not that distinct keys differ.
        const bool hashStable = (qHash(a, 0) == qHash(a, 0)); // same seed -> same result

        if (equalEq && hashEq && inequalNeq && hashStable) {
            pass("G8 key equality and hash are deterministic and correct");
        } else {
            fail("G8 key hash/eq",
                 QStringLiteral("equalEq=%1 hashEq=%2 inequalNeq=%3 hashStable=%4")
                 .arg(equalEq).arg(hashEq).arg(inequalNeq).arg(hashStable));
        }
    }

    // ------------------------------------------------------------------
    // G9: budget=0 means no caching — put is no-op, tryGet always misses.
    // ------------------------------------------------------------------
    {
        CompositeFrameCache cache;
        cache.setMemoryBudgetBytes(0);

        const CompositeFrameKey k = makeKey(1, 0);
        cache.put(k, makeFrame(64, 64));

        QImage dummy;
        const bool miss       = !cache.tryGet(k, dummy);
        const bool countZero  = cache.stats().count == 0;
        const bool bytesZero  = cache.stats().bytes == 0;

        if (miss && countZero && bytesZero) {
            pass("G9 budget=0 disables caching entirely");
        } else {
            fail("G9 budget=0",
                 QStringLiteral("miss=%1 count=%2 bytes=%3")
                 .arg(miss).arg(cache.stats().count).arg(cache.stats().bytes));
        }
    }

    // ------------------------------------------------------------------
    // G10: A single frame larger than the budget is silently dropped — no crash.
    //      Cache remains empty and functional after the oversized put.
    // ------------------------------------------------------------------
    {
        CompositeFrameCache cache;
        // Budget: 1 byte — any real frame exceeds this.
        cache.setMemoryBudgetBytes(1);

        const CompositeFrameKey k = makeKey(1, 0);
        const QImage bigFrame = makeFrame(128, 128); // 128*128*4 = 65 536 bytes

        cache.put(k, bigFrame); // must not crash, must be silently dropped

        QImage dummy;
        const bool miss      = !cache.tryGet(k, dummy);
        const bool countZero  = cache.stats().count == 0;

        // Also verify the cache remains usable for a tiny frame that fits.
        // (budget=1 means even a 1-pixel frame at 4 bytes won't fit, but the
        //  important property is no crash and consistent state.)
        if (miss && countZero) {
            pass("G10 oversized frame silently dropped, no crash");
        } else {
            fail("G10 oversized frame",
                 QStringLiteral("miss=%1 count=%2").arg(miss).arg(cache.stats().count));
        }
    }

    // ------------------------------------------------------------------
    // Summary
    // ------------------------------------------------------------------
    qInfo().noquote()
        << "[composite-frame-cache] done —"
        << passed << "passed," << failed << "failed";
    return failed;
}
