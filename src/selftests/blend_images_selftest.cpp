// src/selftests/blend_images_selftest.cpp
// US-2309: LayerCompositor::blendImages applies large images in row bands on the
// global thread pool. The output must stay byte-identical to the serial loop
// (blendImagesSerial) for every blend mode, and to a stored hash.

#include "../LayerCompositor.h"
#include "SelftestGate.h"

#include <QCryptographicHash>
#include <QImage>
#include <QSemaphore>
#include <QString>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// Befriended by LayerCompositor so the serial reference is reachable.
int runBlendImagesSelftest()
{
    using std::chrono::steady_clock;

    const BlendMode kModes[] = {
        BlendMode::Normal,    BlendMode::Add,        BlendMode::Multiply,
        BlendMode::Screen,    BlendMode::Overlay,    BlendMode::SoftLight,
        BlendMode::HardLight, BlendMode::Difference, BlendMode::Exclusion,
        BlendMode::ColorDodge, BlendMode::ColorBurn, BlendMode::Darken,
        BlendMode::Lighten};
    constexpr int kModeCount = static_cast<int>(sizeof(kModes) / sizeof(kModes[0]));

    // Deterministic pseudo-random image (xorshift32).
    auto makeImage = [](int w, int h, uint32_t seed, bool withAlpha) {
        QImage img(w, h, QImage::Format_ARGB32);
        uint32_t s = seed ? seed : 1u;
        for (int y = 0; y < h; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                line[x] = withAlpha ? s : (s | 0xFF000000u);
            }
        }
        return img;
    };
    auto sameBytes = [](const QImage &a, const QImage &b) {
        return a.format() == b.format() && a.size() == b.size()
            && a.sizeInBytes() == b.sizeInBytes()
            && std::memcmp(a.constBits(), b.constBits(), static_cast<size_t>(a.sizeInBytes())) == 0;
    };
    auto sha = [](const QImage &img) {
        return QCryptographicHash::hash(
                   QByteArray::fromRawData(reinterpret_cast<const char *>(img.constBits()),
                                           static_cast<qsizetype>(img.sizeInBytes())),
                   QCryptographicHash::Sha256).toHex();
    };

    selftest::GateTally tally;

    // ---- G1: banded == serial for every mode / opacity / size / alpha ----
    {
        struct Dim { int bw, bh, tw, th; };
        // 255x257 is under the pixel threshold, 1920x63 reaches the band count
        // test with 0 bands, 1920x127 has 1 band, 1920x128 has 2 bands. The last
        // row mixes base and top sizes (the smaller one wins).
        const Dim dims[] = {
            {1, 1, 1, 1}, {7, 3, 7, 3}, {255, 257, 255, 257}, {1920, 63, 1920, 63},
            {1920, 127, 1920, 127}, {1920, 128, 1920, 128}, {1920, 1080, 1920, 1080},
            {1921, 1081, 1921, 1081}, {1920, 1080, 1900, 1000}};
        const double opacities[] = {0.0, 0.37, 1.0};
        int compared = 0, mismatches = 0, inputDamaged = 0;
        QString firstBad;
        for (const Dim &d : dims) {
            for (int alpha = 0; alpha < 2; ++alpha) {
                const QImage base = makeImage(d.bw, d.bh, 0x1234567u + d.bw, alpha != 0);
                const QImage top = makeImage(d.tw, d.th, 0x9abcdefu + d.th, alpha != 0);
                const QImage baseCopy = base.copy();
                const QImage topCopy = top.copy();
                const QImage baseShared = base;  // shares pixel data with base
                const QImage topShared = top;
                for (BlendMode mode : kModes) {
                    for (double opacity : opacities) {
                        const QImage banded = LayerCompositor::blendImages(base, top, mode, opacity);
                        const QImage serial = LayerCompositor::blendImagesSerial(base, top, mode, opacity);
                        ++compared;
                        if (!sameBytes(banded, serial)) {
                            ++mismatches;
                            if (firstBad.isEmpty())
                                firstBad = QStringLiteral("%1x%2 mode %3 opacity %4 alpha %5")
                                    .arg(d.bw).arg(d.bh).arg(static_cast<int>(mode))
                                    .arg(opacity).arg(alpha);
                        }
                    }
                }
                if (!sameBytes(base, baseCopy) || !sameBytes(top, topCopy)
                    || !sameBytes(baseShared, baseCopy) || !sameBytes(topShared, topCopy))
                    ++inputDamaged;
            }
        }
        // Control: the comparator must see a single flipped byte, or the gate is vacuous.
        QImage ref = makeImage(64, 64, 77u, true);
        QImage flipped = ref.copy();
        flipped.bits()[ref.sizeInBytes() / 2] ^= 1;
        const bool controlOk = sameBytes(ref, ref.copy()) && !sameBytes(ref, flipped);
        std::fprintf(stderr, "[blend-images] G1 compared=%d mismatches=%d inputDamaged=%d control=%d %s\n",
                     compared, mismatches, inputDamaged, controlOk ? 1 : 0,
                     firstBad.toUtf8().constData());
        tally.gate(1, controlOk && compared == 9 * 2 * kModeCount * 3 && mismatches == 0 && inputDamaged == 0,
                   "blendImages differs from blendImagesSerial, damaged an input, or the comparator is blind");
    }

    // ---- G2: stored sha256 per mode (fixture 640x360 -> 5 bands) ----
    {
        // Taken in mode order from the unmodified base-sha 0335b53 exe's LayerCompositor::blendImages
        // (standalone probe, raw in .omc/state/ralph4-results/US-2309-g2-head-constants.log).
        static const char *const kExpected[kModeCount] = {
            "75a4aaf57f8c04a19bda3d4486382a4cb53d825d71be104d16e469f6bf5e7af8",
            "aec5837d9133122420c25bc1132674335cacbec075056923ff10e76b6fbc0ef4",
            "085a13c8d634e13aeadda220d2d0d865476737bc6eef8d2d1faa5c1660e1c314",
            "0de90725fff98f3ca8bb8b5dfb585c57cc9649a0fa0fe2b01377924d1a3eba5d",
            "cd369fa467baa2c58f650192d8ce580a12fc9f33dfa0b7b8edac69127245c845",
            "455c421371e42335b00fac68bcda4d8feda1543808d44029eee73ac48ec713db",
            "c08c898888a593ab29f6aea9e0072c1630f12fea344f867c9cccd129ae3c3dfb",
            "bf48ed866ab8624b78741c6690d54e514df891c8850157d2b4df95b81aa2206a",
            "34a119d05494b9cdb8657700af77812c701f35b0cc83c0230b94046fbe202fa7",
            "68d418ed646511530acd424f1a4b0ca419ffbe59ee4ff2f0b5617900545ef7bd",
            "260fe10e30cdfe069b8e6ae629afafba4222c7bc779998defab8f240215aaf36",
            "409b325f81ec9de97ab83233d7cdea5ca243c9534cd6413ec9954dfa83113eef",
            "9925ebf9e5728bf236a84988d68452e464b1ec673f1f6ad4300b0c0b6af1bb61"};
        const QImage base = makeImage(640, 360, 0xC0FFEEu, true);
        const QImage top = makeImage(640, 360, 0xBADF00Du, true);
        int bad = 0;
        QString report;
        for (int i = 0; i < kModeCount; ++i) {
            const QByteArray gotBanded = sha(LayerCompositor::blendImages(base, top, kModes[i], 0.73));
            const QByteArray gotSerial = sha(LayerCompositor::blendImagesSerial(base, top, kModes[i], 0.73));
            const QByteArray want = QByteArray(kExpected[i]);
            if (gotBanded != want || gotSerial != want) {
                ++bad;
                report += QStringLiteral(" mode%1 banded=%2 serial=%3").arg(i)
                              .arg(QString::fromLatin1(gotBanded), QString::fromLatin1(gotSerial));
            }
        }
        // Control: a different opacity must give a different hash, so a constant
        // table cannot match by accident of an all-equal output.
        const bool control = sha(LayerCompositor::blendImages(base, top, BlendMode::Multiply, 0.73))
                          != sha(LayerCompositor::blendImages(base, top, BlendMode::Multiply, 0.74));
        if (bad) std::fprintf(stderr, "[blend-images] G2%s\n", report.toUtf8().constData());
        tally.gate(2, bad == 0 && control, "per-mode sha256 differs from the stored constants");
    }

    // ---- G3: concurrency, pool re-entrancy and a saturated pool ----
    {
        const QImage base = makeImage(1920, 1080, 0xA5A5A5u, true);
        const QImage top = makeImage(1920, 1080, 0x5A5A5Au, true);
        const QImage expected = LayerCompositor::blendImagesSerial(base, top, BlendMode::Multiply, 0.61);
        const auto deadline = [](int ms) { return steady_clock::now() + std::chrono::milliseconds(ms); };

        // (a) four plain threads at once.
        std::atomic<int> concurrentOk{0};
        {
            std::vector<std::thread> threads;
            for (int i = 0; i < 4; ++i)
                threads.emplace_back([&] {
                    if (sameBytes(LayerCompositor::blendImages(base, top, BlendMode::Multiply, 0.61), expected))
                        ++concurrentOk;
                });
            for (std::thread &t : threads) t.join();
        }
        const bool a = concurrentOk == 4;

        // (b) called from inside a global-pool thread. State is shared and the
        // images are captured by value so a task that finishes after the deadline
        // cannot write to a dead stack frame.
        struct PoolState {
            std::atomic<bool> done{false}, ok{false};
        };
        auto ps = std::make_shared<PoolState>();
        QFuture<void> poolFuture = QtConcurrent::run([ps, base, top, expected, sameBytes] {
            ps->ok = sameBytes(LayerCompositor::blendImages(base, top, BlendMode::Multiply, 0.61), expected);
            ps->done = true;
        });
        const auto bDeadline = deadline(5000);
        while (!ps->done && steady_clock::now() < bDeadline) QThread::msleep(5);
        const bool b = ps->done && ps->ok;
        if (ps->done) poolFuture.waitForFinished();

        // (c) every global-pool thread held on a semaphore; callers from outside
        // the pool and from inside it must still finish by running the bands
        // themselves. State is shared and the threads are detached so a hang
        // is reported as FAIL instead of blocking the run.
        struct Shared {
            QSemaphore release;
            std::atomic<int> started{0};
            std::atomic<bool> outsideDone{false}, outsideOk{false};
            std::atomic<bool> insideDone{false}, insideOk{false};
        };
        auto st = std::make_shared<Shared>();
        QThreadPool *pool = QThreadPool::globalInstance();
        const int poolSize = pool->maxThreadCount();
        for (int i = 0; i < poolSize; ++i) {
            QtConcurrent::run(pool, [st, poolSize, base, top, expected, i, sameBytes] {
                ++st->started;
                if (i == 0) {
                    const auto spin = steady_clock::now() + std::chrono::seconds(5);
                    while (st->started < poolSize && steady_clock::now() < spin) QThread::msleep(1);
                    st->insideOk = sameBytes(
                        LayerCompositor::blendImages(base, top, BlendMode::Multiply, 0.61), expected);
                    st->insideDone = true;
                }
                st->release.acquire();
            });
        }
        const auto sat = deadline(5000);
        while (st->started < poolSize && steady_clock::now() < sat) QThread::msleep(2);
        const bool saturated = st->started == poolSize;
        std::thread([st, base, top, expected, sameBytes] {
            st->outsideOk = sameBytes(
                LayerCompositor::blendImages(base, top, BlendMode::Multiply, 0.61), expected);
            st->outsideDone = true;
        }).detach();
        const auto cDeadline = deadline(5000);
        while (!(st->outsideDone && st->insideDone) && steady_clock::now() < cDeadline) QThread::msleep(5);
        const bool c = saturated && st->outsideDone && st->outsideOk && st->insideDone && st->insideOk;
        st->release.release(poolSize);

        std::fprintf(stderr,
                     "[blend-images] G3 threads=%d concurrent=%d poolInside=%d/%d saturated=%d(%d) "
                     "outside=%d/%d inside=%d/%d\n",
                     QThread::idealThreadCount(), concurrentOk.load(), ps->done.load(), ps->ok.load(),
                     saturated, poolSize, st->outsideDone.load(), st->outsideOk.load(),
                     st->insideDone.load(), st->insideOk.load());
        // Let the blocked pool tasks drain before the next suite runs.
        const bool drained = pool->waitForDone(5000);
        if (!drained) std::fprintf(stderr, "[blend-images] G3 drain timeout\n");
        tally.gate(3, a && b && c && drained, "blendImages hung or differed under concurrency / pool saturation");
    }

    // Timing (information only, no gate): median of 7 calls at 1080p.
    {
        const QImage base = makeImage(1920, 1080, 0x1111u, true);
        const QImage top = makeImage(1920, 1080, 0x2222u, true);
        auto medianMs = [&](bool banded, BlendMode mode) {
            std::vector<double> ms;
            for (int i = 0; i < 7; ++i) {
                const auto t0 = steady_clock::now();
                const QImage out = banded ? LayerCompositor::blendImages(base, top, mode, 1.0)
                                          : LayerCompositor::blendImagesSerial(base, top, mode, 1.0);
                const auto t1 = steady_clock::now();
                if (out.isNull()) return -1.0;
                ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            std::sort(ms.begin(), ms.end());
            return ms[3];
        };
        for (BlendMode mode : {BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay})
            std::fprintf(stderr, "[blend-images] timing 1920x1080 mode=%d serial=%.2f ms banded=%.2f ms\n",
                         static_cast<int>(mode), medianMs(false, mode), medianMs(true, mode));
    }

    return tally.finish("[blend-images]");
}
