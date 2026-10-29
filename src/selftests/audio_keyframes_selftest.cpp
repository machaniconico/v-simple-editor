#include "../AudioKeyframes.h"
#include "../ClipExpressionBindings.h"
#include "../Expression.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../clipanim/ClipAnim.h"
#include "../libavcore/AudioExtract.h"
#include "SelftestRegistry.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>

int runAudioKeyframesSelftest()
{
    int pass = 0, fail = 0;
    const auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++pass : ++fail;
    };
    constexpr int sr = 48000;
    constexpr double fps = 30.0;
    QVector<float> mono(sr * 2, 0.0f);
    for (int start : {sr / 2, sr * 3 / 2})
        for (int i = 0; i < sr / int(fps); ++i) mono[start + i] = 1.0f;
    const auto raw = audiokf::computeEnvelope(mono, sr, fps, 0.0);
    QVector<int> peaks;
    for (int i = 1; i + 1 < raw.rms.size(); ++i)
        if (raw.rms[i] > raw.rms[i - 1] && raw.rms[i] > raw.rms[i + 1]) peaks.append(i);
    gate(1, peaks.size() == 2 && std::abs(peaks.value(0) - 15) <= 1
        && std::abs(peaks.value(1) - 45) <= 1 && raw.rms.size() == 60);

    auto mappedEnvelope = raw;
    mappedEnvelope.startSec = 0.25;
    mappedEnvelope.rms[0] = 0.001f;
    const auto points = audiokf::toKeyframes(mappedEnvelope, 10.0, 110.0, 0.01, 0.5);
    bool mapping = points.size() == 60;
    for (int i = 0; i < points.size(); ++i) {
        const double expected = (i == 15 || i == 45) ? 110.0 : 10.0;
        mapping = mapping && std::abs(points[i].value - expected) < 0.01
            && std::abs(points[i].time - (0.75 + i / fps)) < 1e-12
            && points[i].interpolation == KeyframePoint::Linear;
    }
    // Both VideoPlayer and TimelineFrameRenderer use these clipanim evaluators.
    ClipInfo clip{};
    clip.keyframes.addTrack(audiokf::propertyTrack(QStringLiteral("transform.opacity"), points, 10.0));
    clip.keyframes.addTrack(audiokf::propertyTrack(QStringLiteral("transform.scale"), points, 10.0));
    mapping = mapping && clip.keyframes.hasTrack(QStringLiteral("motion.opacity"))
        && std::abs(clipanim::effectiveOpacityAt(clip, 0.75, 0.5) - 0.1) < 1e-12
        && std::abs(clipanim::effectiveTransformAt(clip, 0.75).videoScale - 10.0) < 1e-12;
    gate(2, mapping);

    double previous = 2.0;
    bool monotonic = true;
    for (double smoothing : {0.0, 20.0, 50.0, 100.0, 200.0}) {
        const auto envelope = audiokf::computeEnvelope(mono, sr, fps, smoothing);
        const double maximum = *std::max_element(envelope.rms.cbegin(), envelope.rms.cend());
        monotonic = monotonic && maximum > 0.0 && maximum < previous;
        previous = maximum;
    }
    gate(3, monotonic);

    ExpressionContext ctx;
    ctx.time = 0.25;
    int calls = 0;
    ctx.audioLevelAtTime = [&](double time) { ++calls; return time; };
    const auto now = Expression::evaluate(QStringLiteral("audioLevel()"), ctx);
    const auto ahead = Expression::evaluate(QStringLiteral("audioLevel(0.5)"), ctx);
    const auto behind = Expression::evaluate(QStringLiteral("audioLevel(-0.5)"), ctx);
    exprbind::ClipExpressionBindings bindings;
    bindings.setExpression(QStringLiteral("transform.opacity"), QStringLiteral("audioLevel() * 100"));
    const double bound = bindings.resolve(QStringLiteral("transform.opacity"), ctx, 42.0);
    ctx.audioLevelAtTime = {};
    const auto silent = Expression::evaluate(QStringLiteral("audioLevel()"), ctx);
    // US-1505: the sampler reaches resolve() through ctx.audioLevelAtTime only (the
    // bindings carry none, formerly a setter on the bindings).
    ctx.audioLevelAtTime = [](double time) { return time * 2.0; };
    const double supplied = bindings.resolve(QStringLiteral("transform.opacity"), ctx, 42.0);
    const auto time = Expression::evaluate(QStringLiteral("time"), ctx);
    const auto ease = Expression::evaluate(QStringLiteral("ease(0.5, 0, 1, 0, 100)"), ctx);
    audiokf::EnvelopeCache cache;
    ctx.audioLevelAtTime = [&](double t) {
        return audiokf::levelAt(*cache.envelope(QStringLiteral("unused-audio.wav")), t);
    };
    bindings.setExpression(QStringLiteral("transform.opacity"), QStringLiteral("time * 100"));
    audiokf::resetInvocationCountForTest();
    bool unused = true;
    for (bool disabled : {false, true}) {
        audiokf::setDisabledForTest(disabled);
        unused = unused && bindings.resolve(QStringLiteral("transform.opacity"), ctx, 42.0) == 25.0
            && audiokf::invocationCountForTest() == 0;
    }
    audiokf::setDisabledForTest(false);
    gate(4, now.success && now.value == 0.25 && ahead.success && ahead.value == 0.75
        && behind.success && behind.value == -0.25 && calls == 4 && bound == 25.0
        && silent.success && silent.value == 0.0 && supplied == 50.0
        && time.success && time.value == 0.25 && ease.success && ease.value == 50.0
        && !Expression::evaluate(QStringLiteral("audioLevel(1, 2)"), ctx).success && unused);

    const auto first = audiokf::computeEnvelope(mono, sr, 29.97, 50.0);
    const auto second = audiokf::computeEnvelope(mono, sr, 29.97, 50.0);
    const auto a = audiokf::toKeyframes(first, -10.0, 20.0, 0.01, -0.5);
    const auto b = audiokf::toKeyframes(second, -10.0, 20.0, 0.01, -0.5);
    bool deterministic = first.rms == second.rms && a.size() == b.size();
    for (int i = 0; i < a.size(); ++i)
        deterministic = deterministic && a[i].time == b[i].time && a[i].value == b[i].value;
    QTemporaryDir temp;
    QString error;
    const QString wav = temp.filePath(QStringLiteral("stereo.wav"));
    // One stereo frame: +0.5 and -0.25 -> mono +0.125, exact PCM16 values.
    QByteArray pcm;
    for (unsigned char byte : {0x00, 0x40, 0x00, 0xe0}) pcm.append(char(byte));
    std::vector<double> decoded;
    int decodedRate = 0;
    const bool wavOk = temp.isValid() && libavcore::writePcm16AsWav(wav, pcm, sr, 2, &error)
        && libavcore::readPcm16WavToMono(wav, decoded, decodedRate, &error)
        && decodedRate == sr && decoded.size() == 1 && decoded[0] == 0.125;
    gate(5, deterministic && wavOk && audiokf::computeEnvelope(mono, 0, fps, 0).rms.isEmpty()
        && audiokf::computeEnvelope(mono, sr, 0, 0).rms.isEmpty()
        && audiokf::levelAt(raw, -0.1) == 0.0 && audiokf::levelAt(raw, 2.0) == 0.0);
    // Exercise the same link lookup, lazy cache and source-time mapping as MainWindow.
    const QString linkedWav = temp.filePath(QStringLiteral("linked.wav"));
    QByteArray linkedPcm;
    linkedPcm.reserve(sr * 2 * 2);
    for (int i = 0; i < sr * 2; ++i) {
        linkedPcm.append(char(0x00));
        linkedPcm.append(i < sr / 2 ? char(0x00) : char(0x40));
    }
    const bool linkedWavOk = temp.isValid()
        && libavcore::writePcm16AsWav(linkedWav, linkedPcm, sr, 1, &error);
    ClipInfo unrelated{};
    unrelated.duration = 0.25;
    unrelated.linkGroup = 8;
    unrelated.filePath = linkedWav;
    ClipInfo linked{};
    linked.duration = 2.0;
    linked.inPoint = 0.5;
    linked.outPoint = 1.5;
    linked.speed = 2.0;
    linked.leadInSec = 0.5;
    linked.linkGroup = 7;
    linked.filePath = linkedWav;
    const QVector<ClipInfo> firstTrack{unrelated};
    const QVector<ClipInfo> secondTrack{unrelated, linked};
    const QVector<const QVector<ClipInfo>*> audioTracks{nullptr, &firstTrack, &secondTrack};
    const auto linkedCache = std::make_shared<audiokf::EnvelopeCache>();
    audiokf::resetInvocationCountForTest();
    const auto unlinked = audiokf::makeLinkedAudioSampler(0, 1.0, audioTracks, linkedCache);
    const auto missing = audiokf::makeLinkedAudioSampler(99, 1.0, audioTracks, linkedCache);
    ExpressionContext linkedContext;
    linkedContext.audioLevelAtTime = audiokf::makeLinkedAudioSampler(7, 1.0, audioTracks, linkedCache);
    const auto ordinary = Expression::evaluate(QStringLiteral("time + 1"), linkedContext);
    bool linkedOk = linkedWavOk && !unlinked && !missing && linkedContext.audioLevelAtTime
        && ordinary.success && ordinary.value == 1.0 && audiokf::invocationCountForTest() == 0;
    // Audio occupies [0.75, 1.25); the target clip starts at 1.0.
    for (double local : {-0.5, 0.25, 0.5}) {
        linkedContext.time = local;
        const auto outside = Expression::evaluate(QStringLiteral("audioLevel()"), linkedContext);
        linkedOk = linkedOk && outside.success && outside.value == 0.0
            && audiokf::invocationCountForTest() == 0;
    }
    linkedContext.time = 0.0;
    const auto inside = Expression::evaluate(QStringLiteral("audioLevel()"), linkedContext);
    const auto repeated = Expression::evaluate(QStringLiteral("audioLevel()"), linkedContext);
    linkedOk = linkedOk && inside.success && std::abs(inside.value - 0.5) < 0.001
        && repeated.success && repeated.value == inside.value
        && audiokf::invocationCountForTest() == 2;
    gate(6, linkedOk);

    // G7 (US-1505): EnvelopeCache decodes outside its mutex. B is cached before the
    // delay is armed; A's decode then sleeps 300 ms (setDecodeDelayForTest) on a worker
    // thread, and once A is inside that sleep, B's cached envelope() returns before A
    // has finished. Detects: decodeMono run under m_mutex (B waits for A).
    const QString wavA = temp.filePath(QStringLiteral("g7-a.wav"));
    const QString wavB = temp.filePath(QStringLiteral("g7-b.wav"));
    QByteArray g7Pcm;
    for (int i = 0; i < sr / 4; ++i) {
        g7Pcm.append(char(0x00));
        g7Pcm.append(char(0x20));
    }
    const bool g7Wavs = temp.isValid() && libavcore::writePcm16AsWav(wavA, g7Pcm, sr, 1, &error)
        && libavcore::writePcm16AsWav(wavB, g7Pcm, sr, 1, &error);
    audiokf::EnvelopeCache g7Cache;
    const bool bCached = g7Wavs && !g7Cache.envelope(wavB)->rms.isEmpty();
    bool aEnteredDelay = false, aDoneWhenBReturned = true, aDoneAtEnd = false, bServed = false;
    {
        struct DelayReset {
            ~DelayReset() { audiokf::setDecodeDelayForTest(0); }
        } delayReset;
        audiokf::setDecodeDelayForTest(300);
        const int entriesBefore = audiokf::decodeDelayEntriesForTest();
        std::atomic<bool> aDone{false};
        std::thread decodeA([&] {
            g7Cache.envelope(wavA);
            aDone = true;
        });
        QElapsedTimer waitDelay;
        waitDelay.start();
        while (audiokf::decodeDelayEntriesForTest() == entriesBefore && waitDelay.elapsed() < 5000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        aEnteredDelay = audiokf::decodeDelayEntriesForTest() > entriesBefore;
        const auto bEnvelope = g7Cache.envelope(wavB);
        aDoneWhenBReturned = aDone.load();
        bServed = bEnvelope && !bEnvelope->rms.isEmpty();
        decodeA.join();
        aDoneAtEnd = aDone.load();
    }
    std::fprintf(stderr, "audio-keyframes G7 wavs=%d bCached=%d aEnteredDelay=%d bServed=%d "
                 "aDoneWhenBReturned=%d (expect 0) aDoneAtEnd=%d\n", int(g7Wavs), int(bCached),
                 int(aEnteredDelay), int(bServed), int(aDoneWhenBReturned), int(aDoneAtEnd));
    gate(7, bCached && aEnteredDelay && bServed && !aDoneWhenBReturned && aDoneAtEnd);

    // G8 (US-1709): a Timeline loaded from a project file (ProjectFile::load ->
    // restoreFromProject, the Timeline a file-based render job uses) gets its
    // audioLevel() envelope from buildExpressionAudioEnv + setExpressionAudioEnv:
    // the V1 clip linked to a loud audio clip samples > 0, linked to a silent one 0.
    // Control: the same loaded Timeline without the set has no sampler (level 0).
    {
        int argc = 1;
        char arg0[] = "audio-keyframes";
        char *argv[] = {arg0, nullptr};
        std::unique_ptr<QApplication> ownedApp;
        if (!QCoreApplication::instance()) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            ownedApp = std::make_unique<QApplication>(argc, argv);
        }
        const bool haveApp = qobject_cast<QApplication *>(QCoreApplication::instance()) != nullptr;
        const QString loudWav = temp.filePath(QStringLiteral("g8-loud.wav"));
        const QString silentWav = temp.filePath(QStringLiteral("g8-silent.wav"));
        QByteArray loudPcm, silentPcm;
        for (int i = 0; i < sr * 2; ++i) {
            loudPcm.append(char(0x00));
            loudPcm.append(char(0x40));
            silentPcm.append(char(0x00));
            silentPcm.append(char(0x00));
        }
        const bool g8Wavs = temp.isValid()
            && libavcore::writePcm16AsWav(loudWav, loudPcm, sr, 1, &error)
            && libavcore::writePcm16AsWav(silentWav, silentPcm, sr, 1, &error);
        // Loads the project through the file and samples audioLevel() of V1 at 0.5 s.
        const auto levelFromFile = [&](const QString &audioPath, const QString &name,
                                       bool publish, bool *hasSampler) {
            *hasSampler = false;
            ClipInfo video{};
            video.displayName = QStringLiteral("g8 video");
            video.duration = video.outPoint = 2.0;
            video.linkGroup = 11;
            video.expressions.setExpression(QStringLiteral("transform.opacity"),
                                            QStringLiteral("audioLevel() * 100"));
            ClipInfo audio{};
            audio.duration = audio.outPoint = 2.0;
            audio.linkGroup = 11;
            audio.filePath = audioPath;
            ProjectData saved;
            saved.videoTracks = QVector<QVector<ClipInfo>>{{video}};
            saved.audioTracks = QVector<QVector<ClipInfo>>{{audio}};
            const QString projectPath = temp.filePath(name + QStringLiteral(".veditor"));
            ProjectData loaded;
            if (!ProjectFile::save(projectPath, saved) || !ProjectFile::load(projectPath, loaded))
                return -1.0;
            Timeline timeline;
            timeline.restoreFromProject(loaded.videoTracks, loaded.audioTracks, 0.0, -1.0, -1.0, 100);
            if (publish)
                timeline.setExpressionAudioEnv(
                    timeline.buildExpressionAudioEnv(audiokf::sharedEnvelopeCache()));
            const auto *v1 = timeline.videoTracks().value(0);
            if (!v1 || v1->clips().isEmpty()) return -1.0;
            const auto sampler = timeline.expressionAudioLevelSampler(v1->clips().first(), 0.0);
            *hasSampler = bool(sampler);
            ExpressionContext context;
            context.time = 0.5;
            context.audioLevelAtTime = sampler;
            const auto level = Expression::evaluate(QStringLiteral("audioLevel()"), context);
            return level.success ? level.value : -1.0;
        };
        bool loudSampler = false, silentSampler = false, unsetSampler = false;
        const double loud = haveApp && g8Wavs
            ? levelFromFile(loudWav, QStringLiteral("g8-loud"), true, &loudSampler) : -1.0;
        const double silent = haveApp && g8Wavs
            ? levelFromFile(silentWav, QStringLiteral("g8-silent"), true, &silentSampler) : -1.0;
        const double unset = haveApp && g8Wavs
            ? levelFromFile(loudWav, QStringLiteral("g8-unset"), false, &unsetSampler) : -1.0;
        const bool oneCache = audiokf::sharedEnvelopeCache()
            && audiokf::sharedEnvelopeCache() == audiokf::sharedEnvelopeCache();
        std::fprintf(stderr, "audio-keyframes G8 app=%d wavs=%d loud=%.4f sampler=%d silent=%.4f "
                     "sampler=%d unset=%.4f sampler=%d (expect 0 / 0) oneCache=%d\n", int(haveApp),
                     int(g8Wavs), loud, int(loudSampler), silent, int(silentSampler), unset,
                     int(unsetSampler), int(oneCache));
        gate(8, haveApp && g8Wavs && loudSampler && loud > 0.1 && silentSampler && silent == 0.0
            && !unsetSampler && unset == 0.0 && oneCache);

        // G9-G11 (US-1710): publishing the expression audio snapshot (setExpressionAudioEnv)
        // warms the envelopes that audioLevel() will read on audiokf::prefetchPool(). Each
        // spec is {linkGroup, expression code} for a V1 clip and {linkGroup, wav path} for an
        // audio clip; the env is published on the Timeline with a local cache.
        struct VideoSpec { int linkGroup; QString code; };
        struct AudioSpec { int linkGroup; QString path; };
        const auto makeWav = [&](const QString &name) {
            QByteArray pcm;
            for (int i = 0; i < sr; ++i) {
                pcm.append(char(0x00));
                pcm.append(char(0x40));
            }
            const QString path = temp.filePath(name);
            return libavcore::writePcm16AsWav(path, pcm, sr, 1, &error) ? path : QString();
        };
        const auto loadTimeline = [&](Timeline &timeline, const QVector<VideoSpec> &videos,
                                      const QVector<AudioSpec> &audios, bool separateAudioTracks = false) {
            QVector<ClipInfo> videoClips, audioClips;
            for (const VideoSpec &spec : videos) {
                ClipInfo video{};
                video.displayName = QStringLiteral("g9 video");
                video.duration = video.outPoint = 1.0;
                video.linkGroup = spec.linkGroup;
                video.expressions.setExpression(QStringLiteral("transform.opacity"), spec.code);
                videoClips.append(video);
            }
            for (const AudioSpec &spec : audios) {
                ClipInfo audio{};
                audio.duration = audio.outPoint = 1.0;
                audio.linkGroup = spec.linkGroup;
                audio.filePath = spec.path;
                audioClips.append(audio);
            }
            QVector<QVector<ClipInfo>> audioTracks;
            if (separateAudioTracks) {
                for (const ClipInfo &audio : audioClips)
                    audioTracks.append(QVector<ClipInfo>{audio});
            } else {
                audioTracks.append(audioClips);
            }
            timeline.restoreFromProject(QVector<QVector<ClipInfo>>{videoClips}, audioTracks, 0.0, -1.0,
                                        -1.0, 100);
        };
        struct DelayReset {
            ~DelayReset() { audiokf::setDecodeDelayForTest(0); }
        } delayReset;
        const QString audioLevelCode = QStringLiteral("audioLevel() * 100");

        // G9: two audioLevel() clips; after the publish both envelopes get cached in the
        // background, so the first sampler call is fast and decodes nothing more.
        // Control: the same sampler over a fresh cache that was never warmed pays the full
        // 300 ms delay (what the first call costs without the prefetch).
        const QString g9A = makeWav(QStringLiteral("g9-a.wav"));
        const QString g9B = makeWav(QStringLiteral("g9-b.wav"));
        bool g9Warm = false, g9Fast = false, g9NoMore = false, g9ControlSlow = false;
        double g9FirstMs = -1.0, g9ControlMs = -1.0;
        int g9Entries = 0, g9Invocations = 0;
        if (haveApp && !g9A.isEmpty() && !g9B.isEmpty()) {
            Timeline timeline;
            loadTimeline(timeline, {{21, audioLevelCode}, {22, audioLevelCode}},
                         {{21, g9A}, {22, g9B}});
            const auto cache = std::make_shared<audiokf::EnvelopeCache>();
            audiokf::setDecodeDelayForTest(300);
            audiokf::resetInvocationCountForTest();
            const int entriesBefore = audiokf::decodeDelayEntriesForTest();
            timeline.setExpressionAudioEnv(timeline.buildExpressionAudioEnv(cache));
            QElapsedTimer wait;
            wait.start();
            while (!(audiokf::decodeDelayEntriesForTest() == entriesBefore + 2
                     && cache->isCachedForTest(g9A) && cache->isCachedForTest(g9B))
                   && wait.elapsed() < 5000)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            g9Warm = audiokf::decodeDelayEntriesForTest() == entriesBefore + 2
                && cache->isCachedForTest(g9A) && cache->isCachedForTest(g9B);
            const ClipInfo &first = timeline.videoTracks().value(0)->clips().first();
            const auto sampler = timeline.expressionAudioLevelSampler(first, 0.0);
            QElapsedTimer firstCall;
            firstCall.start();
            const int invBefore = audiokf::invocationCountForTest();
            const double level = sampler ? sampler(0.5) : -1.0;
            g9FirstMs = double(firstCall.nsecsElapsed()) / 1e6;
            g9Invocations = audiokf::invocationCountForTest() - invBefore;
            g9Fast = sampler && level > 0.1 && g9FirstMs < 50.0;
            g9Entries = audiokf::decodeDelayEntriesForTest() - entriesBefore;
            g9NoMore = g9Entries == 2;
            // Control: no publish, no prefetch -> the first call decodes (>= 300 ms).
            const auto coldCache = std::make_shared<audiokf::EnvelopeCache>();
            audiokf::ExprAudioEnv coldEnv;
            ClipInfo coldAudio{};
            coldAudio.duration = coldAudio.outPoint = 1.0;
            coldAudio.linkGroup = 21;
            coldAudio.filePath = g9A;
            coldEnv.audioClipsByTrack = QVector<QVector<ClipInfo>>{{coldAudio}};
            coldEnv.cache = coldCache;
            const auto coldSampler = audiokf::makeLinkedAudioSamplerFrom(coldEnv, 21, 0.0);
            QElapsedTimer coldCall;
            coldCall.start();
            if (coldSampler) coldSampler(0.5);
            g9ControlMs = double(coldCall.nsecsElapsed()) / 1e6;
            g9ControlSlow = g9ControlMs >= 300.0;
            audiokf::setDecodeDelayForTest(0);
        }
        audiokf::prefetchPool().waitForDone();
        std::fprintf(stderr, "audio-keyframes G9 warm=%d firstCallMs=%.2f (expect < 50) entriesDelta=%d "
                     "(expect 2) invocationsDelta=%d (expect 1) controlColdMs=%.2f (expect >= 300)\n", int(g9Warm),
                     g9FirstMs, g9Entries, g9Invocations, g9ControlMs);
        gate(9, haveApp && g9Warm && g9Fast && g9NoMore && g9ControlSlow);

        // G10: the last shared_ptr to the cache is dropped while a prefetch worker is inside
        // its (delayed) decode. The worker holds the cache through weak_ptr::lock(), so the
        // pool drains within 5 s with no deadlock or crash (the cache dies on the worker).
        const QString g10A = makeWav(QStringLiteral("g10-a.wav"));
        bool g10Entered = false, g10Drained = false;
        int g10Entries = 0;
        if (haveApp && !g10A.isEmpty()) {
            Timeline timeline;
            loadTimeline(timeline, {{41, audioLevelCode}}, {{41, g10A}});
            auto cache = std::make_shared<audiokf::EnvelopeCache>();
            audiokf::setDecodeDelayForTest(300);
            const int entriesBefore = audiokf::decodeDelayEntriesForTest();
            timeline.setExpressionAudioEnv(timeline.buildExpressionAudioEnv(cache));
            QElapsedTimer wait;
            wait.start();
            while (audiokf::decodeDelayEntriesForTest() == entriesBefore && wait.elapsed() < 5000)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            g10Entered = audiokf::decodeDelayEntriesForTest() > entriesBefore;
            timeline.setExpressionAudioEnv(nullptr);
            cache.reset();
            g10Drained = audiokf::prefetchPool().waitForDone(5000);
            g10Entries = audiokf::decodeDelayEntriesForTest() - entriesBefore;
            audiokf::setDecodeDelayForTest(0);
        }
        std::fprintf(stderr, "audio-keyframes G10 enteredDecode=%d drainedWithin5s=%d entriesDelta=%d\n",
                     int(g10Entered), int(g10Drained), g10Entries);
        gate(10, haveApp && g10Entered && g10Drained && g10Entries == 1);

        // G11 (MJ3): only the audio an audioLevel() expression reads is decoded.
        // (a) no audioLevel expression -> the decode counter does not move (control: with one
        // audioLevel clip the same counter moves by 1, so the counter is live).
        // (b) one audioLevel clip (linkGroup 51) next to an unrelated audio clip (52): only
        // the linked one is cached.
        const QString g11A = makeWav(QStringLiteral("g11-a.wav"));
        const QString g11B = makeWav(QStringLiteral("g11-b.wav"));
        int g11PlainDelta = -1, g11ControlDelta = -1;
        bool g11OnlyLinked = false, g11OtherCached = true;
        if (haveApp && !g11A.isEmpty() && !g11B.isEmpty()) {
            audiokf::setDecodeDelayForTest(1);
            {
                Timeline timeline;
                loadTimeline(timeline, {{51, QStringLiteral("time * 100")}}, {{51, g11A}, {52, g11B}}, true);
                const auto cache = std::make_shared<audiokf::EnvelopeCache>();
                const int before = audiokf::decodeDelayEntriesForTest();
                timeline.setExpressionAudioEnv(timeline.buildExpressionAudioEnv(cache));
                audiokf::prefetchPool().waitForDone();
                g11PlainDelta = audiokf::decodeDelayEntriesForTest() - before;
            }
            {
                Timeline timeline;
                loadTimeline(timeline, {{51, audioLevelCode}}, {{51, g11A}, {52, g11B}}, true);
                const auto cache = std::make_shared<audiokf::EnvelopeCache>();
                const int before = audiokf::decodeDelayEntriesForTest();
                timeline.setExpressionAudioEnv(timeline.buildExpressionAudioEnv(cache));
                audiokf::prefetchPool().waitForDone();
                g11ControlDelta = audiokf::decodeDelayEntriesForTest() - before;
                g11OnlyLinked = cache->isCachedForTest(g11A);
                g11OtherCached = cache->isCachedForTest(g11B);
            }
            audiokf::setDecodeDelayForTest(0);
        }
        std::fprintf(stderr, "audio-keyframes G11 noExprDelta=%d (expect 0) oneExprDelta=%d (expect 1) "
                     "linkedCached=%d otherCached=%d (expect 0)\n", g11PlainDelta, g11ControlDelta,
                     int(g11OnlyLinked), int(g11OtherCached));
        gate(11, haveApp && g11PlainDelta == 0 && g11ControlDelta == 1 && g11OnlyLinked
            && !g11OtherCached);
    }

    // The prefetch workers of G9-G11 are drained before the tally (no aboutToQuit here).
    audiokf::prefetchPool().waitForDone();

    selftests::reportTally(pass, fail, 0);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL, 0 SKIP\n", pass, fail);
    return fail;
}
