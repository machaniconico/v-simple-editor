#include "AudioExportMix.h"
#include "ExportDialog.h"
#include "RenderQueue.h"
#include "Timeline.h"
#include "libavcore/AudioExtract.h"
#include "playback/AudioPan.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace audiofxexport {
// Thread-local instrumentation: export tests cannot alter another worker.
thread_local bool masterEqBypass = false;
thread_local quint64 masterEqPasses = 0;
void setMasterEqBypassForTest(bool bypass)
{
    masterEqBypass = bypass;
    masterEqPasses = 0;
}
quint64 masterEqPassesForTest() { return masterEqPasses; }
} // namespace audiofxexport

namespace {
std::atomic<int> s_buildMixPlanGuiThreadCalls{0};
std::atomic<int> s_buildMixPlanOtherThreadCalls{0};

QString volumeExpressionForEntry(const PlaybackEntry &entry)
{
    QVector<AudioGainPoint> env = audioexport::sortedDedupedEnvelope(entry.volumeEnvelope);
    if (env.isEmpty())
        return audioexport::ffmpegNumber(qBound(0.0, entry.volume, 2.0));

    auto linearExpr = [](const AudioGainPoint &a,
                         const AudioGainPoint &b) {
        const double span = b.time - a.time;
        if (span <= 0.000001)
            return audioexport::ffmpegNumber(b.gain);
        return QStringLiteral("(%1+(%2-%1)*(t-%3)/%4)")
            .arg(audioexport::ffmpegNumber(a.gain),
                 audioexport::ffmpegNumber(b.gain),
                 audioexport::ffmpegNumber(a.time),
                 audioexport::ffmpegNumber(span));
    };

    QString expr = audioexport::ffmpegNumber(env.last().gain);
    for (int i = env.size() - 2; i >= 0; --i) {
        const QString segment = linearExpr(env[i], env[i + 1]);
        expr = QStringLiteral("if(lt(t,%1),%2,%3)")
            .arg(audioexport::ffmpegNumber(env[i + 1].time), segment, expr);
    }
    expr = QStringLiteral("if(lt(t,%1),%2,%3)")
        .arg(audioexport::ffmpegNumber(env.first().time),
             audioexport::ffmpegNumber(env.first().gain),
             expr);
    return expr;
}

QString exportAudioFilterChainForEntry(int inputIndex,
                                       const PlaybackEntry &entry,
                                       AudioChannelMode channelMode,
                                       bool reversed)
{
    const int delayMs = qMax(0, qRound(entry.timelineStart * 1000.0));
    return buildExportAudioMixEntryFilterChain(
        inputIndex,
        audioexport::ffmpegNumber(entry.clipIn),
        audioexport::ffmpegNumber(entry.clipOut),
        delayMs,
        volumeExpressionForEntry(entry),
        channelMode,
        reversed,
        entry.speed,
        entry.leadInType,
        entry.leadInDuration,
        entry.trailOutType,
        entry.trailOutDuration);
}
} // namespace

QString audioexport::findFfmpegBinary()
{
    QString path = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!path.isEmpty())
        return path;

    const QStringList searchPaths = {
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/bin")
    };
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"), searchPaths);
}

QString audioexport::ffmpegNumber(double value)
{
    if (!std::isfinite(value))
        value = 0.0;
    return QString::number(value, 'f', 6);
}

QVector<AudioGainPoint> audioexport::sortedDedupedEnvelope(QVector<AudioGainPoint> env)
{
    std::sort(env.begin(), env.end(),
              [](const AudioGainPoint &a, const AudioGainPoint &b) {
                  return a.time < b.time;
              });
    QVector<AudioGainPoint> out;
    out.reserve(env.size());
    for (const AudioGainPoint &point : env) {
        AudioGainPoint p;
        p.time = qMax(0.0, point.time);
        p.gain = qBound(0.0, point.gain, 2.0);
        if (!out.isEmpty() && std::abs(out.last().time - p.time) <= 0.001) {
            out.last().gain = qMin(out.last().gain, p.gain);
        } else {
            out.append(p);
        }
    }
    return out;
}

QString audioexport::nextExportAudioMixPath()
{
    static int serial = 0;
    const QString dirPath = QDir::tempPath()
        + QStringLiteral("/v-simple-editor-audio-mix");
    QDir().mkpath(dirPath);
    return dirPath
        + QStringLiteral("/ducking_mix_%1_%2.m4a")
              .arg(QDateTime::currentMSecsSinceEpoch())
              .arg(++serial);
}

bool audioexport::runFfmpegForAudioMix(const QStringList &args, QString *error)
{
    const QString ffmpeg = findFfmpegBinary();
    if (ffmpeg.isEmpty()) {
        if (error)
            *error = QStringLiteral("ffmpeg が見つからないため、ダッキング済み audio mix を作成できません。");
        return false;
    }

    QProcess process;
    process.start(ffmpeg, args);
    if (!process.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("ffmpeg を起動できませんでした。");
        return false;
    }
    process.waitForFinished(-1);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("audio mix の作成に失敗しました。\n%1")
                .arg(QString::fromUtf8(process.readAllStandardError()));
        }
        return false;
    }
    return true;
}

int audioexport::buildMixPlanGuiThreadCallsForTest() { return s_buildMixPlanGuiThreadCalls.load(); }
int audioexport::buildMixPlanOtherThreadCallsForTest() { return s_buildMixPlanOtherThreadCalls.load(); }

audioexport::MixPlan audioexport::buildMixPlan(Timeline *timeline, AudioMixer *mixer,
                                               const MixOptions &options)
{
    const QCoreApplication *app = QCoreApplication::instance();
    if (app && QThread::currentThread() == app->thread()) ++s_buildMixPlanGuiThreadCalls;
    else ++s_buildMixPlanOtherThreadCalls;
    MixPlan plan;
    if (!timeline) {
        plan.passthrough = true;
        return plan;
    }
    if (mixer) {
        plan.masterChain = mixer->masterChain();
        for (int track = 0; track < timeline->audioTracks().size(); ++track) {
            const trackfx::Chain chain = mixer->trackChain(track);
            plan.trackChains.insert(track, chain);
            plan.legacyEqConfigs.insert(track, mixer->trackEqConfig(track));
            plan.legacyEqCoeffs.insert(track, mixer->trackEqCoeffs(track));
            plan.legacyEqEnabled.insert(track, mixer->trackEqEnabled(track));
            plan.trackGains.insert(track, mixer->trackGain(track));
            // MIXER-DOCK: non-unity gain needs the existing post-DSP gain path,
            // including when no track effect is enabled.
            plan.useTrackFx = plan.useTrackFx || mixer->trackGain(track) != 1.0;
            plan.useTrackFx = plan.useTrackFx || chain.eqEnabled || chain.compEnabled
                || chain.reverbEnabled || chain.nrEnabled || chain.deesser.enabled
                || (plan.legacyEqEnabled.value(track) && !plan.legacyEqConfigs.value(track).isDefault());
        }
    }
    plan.useMasterEq = (plan.masterChain.eqEnabled || plan.masterChain.deesser.enabled)
        && !audiofxexport::masterEqBypass;
    if (options.forcedOutputPath.isEmpty() && !plan.useTrackFx && !plan.useMasterEq
        && loudnessauto::timelineAudioMatchesPassthrough(timeline)) {
        plan.passthrough = true;
        return plan;
    }

    const QVector<PlaybackEntry> entries = timeline->computeAudioPlaybackSequence();
    plan.renderInPlace = !options.forcedOutputPath.isEmpty();
    plan.durationSeconds = qMax(0.001, plan.renderInPlace
        ? options.forcedDurationSeconds : timeline->totalDuration());
    plan.outputPath = options.forcedOutputPath.isEmpty()
        ? nextExportAudioMixPath() : options.forcedOutputPath;
    plan.audioKbps = options.audioKbps;

    const QVector<bool> reversedFlags =
        audioReversedFlagsForPlaybackEntries(entries);
    plan.entries.reserve(entries.size());
    plan.reversed.reserve(entries.size());
    plan.channelModes.reserve(entries.size());
    for (int entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
        const PlaybackEntry &entry = entries[entryIndex];
        if (entry.audioMuted)
            continue;
        if (entry.timelineEnd <= entry.timelineStart)
            continue;
        if (entry.clipOut <= entry.clipIn)
            continue;
        if (!QFileInfo::exists(entry.filePath))
            continue;
        const bool reversed = reversedFlags.value(entryIndex, false);
        if (reversed && entry.clipOut - entry.clipIn > 10.0 * 60.0) {
            qWarning().noquote()
                << QStringLiteral("音声の逆再生クリップが10分の上限を超えたため、書き出しでは無音にします: %1")
                       .arg(entry.filePath);
            continue;
        }
        plan.entries.append(entry);
        plan.reversed.append(reversed);
        plan.channelModes.append(audioChannelModeForPlaybackEntry(entry));
    }
    return plan;
}

QString audioexport::executeMixPlan(const MixPlan &plan, const std::function<bool()> &cancelled,
                                    QString *error)
{
    if (plan.passthrough)
        return {};
    const QVector<PlaybackEntry> &validEntries = plan.entries;
    const double durationSeconds = plan.durationSeconds;
    const bool renderInPlace = plan.renderInPlace;
    const QString &outputPath = plan.outputPath;
    const auto runFfmpeg = [&](const QStringList &args) {
        if (cancelled && cancelled()) {
            if (error) *error = QStringLiteral("音声ミックスの作成がキャンセルされました。");
            return false;
        }
        return runFfmpegForAudioMix(args, error);
    };
    // Audio-only export uses a PCM intermediate so lossy encoding happens only
    // at the final output. Keep the default video-export arguments unchanged.
    const bool pcmIntermediate = renderInPlace
        && QFileInfo(outputPath).suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0;
    QStringList outputArgs;
    if (pcmIntermediate) {
        outputArgs << QStringLiteral("-c:a") << QStringLiteral("pcm_s16le");
    } else {
        outputArgs << exportaudio::aacAudioArgs(plan.audioKbps)
                   << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    }
    outputArgs << outputPath;

    // Default path retains precisely the original single-pass arguments.
    // Enabled master EQ is applied once to the final amix output, including
    // every track's clip/track gain and pan, using the playback EQ processor.
    const auto finishMixUncleaned = [&](QStringList mixArgs) -> QString {
        if (!plan.useMasterEq) {
            mixArgs << outputArgs;
            return runFfmpeg(mixArgs) ? outputPath : QString();
        }
        ++audiofxexport::masterEqPasses;
        QTemporaryDir masterTemporary;
        if (!masterTemporary.isValid()) {
            if (error) *error = QStringLiteral("マスター EQ の一時フォルダーを作成できません。");
            return {};
        }
        const QString mixPath = masterTemporary.filePath(QStringLiteral("master.wav"));
        mixArgs << QStringLiteral("-ar") << QString::number(AudioMixer::kSampleRateHz)
                << QStringLiteral("-ac") << QString::number(AudioMixer::kChannels)
                << QStringLiteral("-c:a") << QStringLiteral("pcm_f32le") << mixPath;
        if (!runFfmpeg(mixArgs)) return {};
        std::vector<float> samples;
        int rate = 0, channels = 0;
        if (!libavcore::readWavFloatInterleaved(mixPath, &samples, &rate, &channels, error))
            return {};
        if (rate != AudioMixer::kSampleRateHz || channels != AudioMixer::kChannels) {
            if (error) *error = QStringLiteral("マスター音声の形式が不正です。");
            return {};
        }
        AudioMixer::MasterEqFilter masterProcessor;
        masterProcessor.setEq(plan.masterChain.eqEnabled ? plan.masterChain.eq : trackfx::EqSettings{});
        masterProcessor.setDeEsser(plan.masterChain.deesser);
        const size_t frames = samples.size() / channels;
        for (size_t frame = 0; frame < frames; frame += 4096)
            masterProcessor.process(samples.data() + frame * channels,
                static_cast<int>(std::min<size_t>(4096, frames - frame)));
        if (!libavcore::writeWavFloatInterleaved(mixPath, samples, rate, channels, error))
            return {};
        QStringList encodeArgs{QStringLiteral("-y"), QStringLiteral("-i"), mixPath,
                               QStringLiteral("-vn")};
        encodeArgs << outputArgs;
        return runFfmpeg(encodeArgs) ? outputPath : QString();
    };
    // Whoever creates the intermediate removes it again when the mix fails: ffmpeg may have
    // written part of outputPath, and the caller only gets an empty path back.
    const auto finishMix = [&](QStringList mixArgs) -> QString {
        QString result = finishMixUncleaned(mixArgs);
        if (exportaudio::failAfterWriteForTest && !result.isEmpty()) {
            exportaudio::outputExistedBeforeCleanupForTest = QFile::exists(outputPath);
            if (error) *error = QStringLiteral("test: forced failure after write");
            result.clear();
        }
        if (result.isEmpty())
            QFile::remove(outputPath);
        return result;
    };

    QStringList args;
    args << QStringLiteral("-y");
    for (const PlaybackEntry &entry : validEntries)
        args << QStringLiteral("-i") << entry.filePath;

    if (validEntries.isEmpty()) {
        args << QStringLiteral("-f") << QStringLiteral("lavfi")
             << QStringLiteral("-i")
             << QStringLiteral("anullsrc=channel_layout=stereo:sample_rate=48000")
             << QStringLiteral("-t") << ffmpegNumber(durationSeconds)
             << QStringLiteral("-vn");
        return finishMix(args);
    }

    if (plan.useTrackFx) {
        // Settings were snapshot into the plan before starting any potentially long render.
        // Every track (including dry tracks) remains float until the master mix.
        QTemporaryDir temporary;
        if (!temporary.isValid()) {
            if (error) *error = QStringLiteral("トラック音声の一時フォルダーを作成できません。");
            return {};
        }
        // Process source clips in timeline order, retaining each track's DSP
        // histories across clips. Gaps/delays never enter the DSP or NR detector.
        QVector<int> orderedEntries;
        for (int i = 0; i < validEntries.size(); ++i)
            orderedEntries.append(i);
        std::stable_sort(orderedEntries.begin(), orderedEntries.end(), [&](int a, int b) {
            return validEntries[a].timelineStart < validEntries[b].timelineStart;
        });
        QMap<int, QVector<int>> entriesByTrack;
        for (int i : orderedEntries)
            entriesByTrack[validEntries[i].sourceTrack].append(i);
        QVector<QString> clipPaths(validEntries.size());
        for (auto it = entriesByTrack.cbegin(); it != entriesByTrack.cend(); ++it) {
            const int track = it.key();
            std::vector<DspEntry> buffers;
            buffers.reserve(it.value().size());
            for (int i : it.value()) {
                const PlaybackEntry &entry = validEntries[i];
                const QString clipPath = temporary.filePath(QStringLiteral("clip_%1.wav").arg(i));
                const QString filter = buildExportAudioMixEntryPreFxFilterChain(0,
                    ffmpegNumber(entry.clipIn), ffmpegNumber(entry.clipOut),
                    plan.channelModes.value(i, AudioChannelMode::Stereo), plan.reversed.value(i, false),
                    entry.speed);
                QStringList clipArgs{QStringLiteral("-y"), QStringLiteral("-i"), entry.filePath};
                clipArgs << QStringLiteral("-filter_complex") << filter
                         << QStringLiteral("-map") << QStringLiteral("[prefx0]")
                         << QStringLiteral("-vn") << QStringLiteral("-c:a") << QStringLiteral("pcm_f32le")
                         << clipPath;
                if (!runFfmpeg(clipArgs)) return {};
                std::vector<float> samples;
                int rate = 0, channels = 0;
                if (!libavcore::readWavFloatInterleaved(clipPath, &samples, &rate, &channels, error))
                    return {};
                if (rate != AudioMixer::kSampleRateHz || channels != AudioMixer::kChannels) {
                    if (error) *error = QStringLiteral("トラック音声の形式が不正です。");
                    return {};
                }
                buffers.push_back({qRound64(entry.timelineStart * AudioMixer::kSampleRateHz),
                                   std::move(samples)});
                clipPaths[i] = clipPath;
            }
            processTrackEntries(buffers, plan.trackChains.value(track),
                plan.legacyEqCoeffs.value(track), plan.legacyEqEnabled.value(track, false),
                plan.legacyEqConfigs.value(track).preamp);
            for (size_t index = 0; index < buffers.size(); ++index) {
                if (!libavcore::writeWavFloatInterleaved(clipPaths[it.value()[static_cast<int>(index)]],
                        buffers[index].samples, AudioMixer::kSampleRateHz, AudioMixer::kChannels, error))
                    return {};
            }
        }
        QStringList finalArgs{QStringLiteral("-y")};
        QStringList finalInputs, finalChains;
        for (int i = 0; i < validEntries.size(); ++i) {
            const PlaybackEntry &entry = validEntries[i];
            finalArgs << QStringLiteral("-i") << clipPaths[i];
            const double clipDuration = std::isfinite(entry.speed) && entry.speed > 0.0
                ? qMax(0.0, (entry.clipOut - entry.clipIn) / entry.speed)
                : qMax(0.0, entry.clipOut - entry.clipIn);
            QString post = buildExportAudioMixEntryPostFxFilterChain(i,
                qMax(0, qRound(entry.timelineStart * 1000.0)),
                volumeExpressionForEntry(entry), clipDuration,
                entry.leadInType, entry.leadInDuration, entry.trailOutType, entry.trailOutDuration);
            // Static track gain and clip balance also belong after the DSP.
            const auto pan = audiopan::balanceGains(entry.pan);
            const double gain = plan.trackGains.value(entry.sourceTrack, 1.0);
            if (gain != 1.0 || entry.pan != 0.0) {
                post.insert(QStringLiteral("[%1:a]").arg(i).size(),
                    QStringLiteral("pan=stereo|c0=%1*c0|c1=%2*c1,")
                        .arg(ffmpegNumber(gain * pan.left), ffmpegNumber(gain * pan.right)));
            }
            finalChains << post;
            if (renderInPlace) {
                finalChains << QStringLiteral("[a%1]asetpts=N/SR/TB[rip%1]").arg(i);
                finalInputs << QStringLiteral("[rip%1]").arg(i);
            } else {
                finalInputs << QStringLiteral("[a%1]").arg(i);
            }
        }
        finalChains << QStringLiteral("%1amix=inputs=%2:normalize=0:duration=longest,"
                                      "%4atrim=duration=%3,asetpts=PTS-STARTPTS[aout]")
            .arg(finalInputs.join(QString()), QString::number(finalInputs.size()),
                 ffmpegNumber(durationSeconds),
                 renderInPlace ? QStringLiteral("asetpts=N/SR/TB,apad,") : QString());
        const QString finalFilter = finalChains.join(QLatin1Char(';'));
        finalArgs << QStringLiteral("-filter_complex") << finalFilter
                  << QStringLiteral("-map") << QStringLiteral("[aout]") << QStringLiteral("-vn");
        return finishMix(finalArgs);
    }

    QStringList chains;
    QStringList mixInputs;
    for (int i = 0; i < validEntries.size(); ++i) {
        const PlaybackEntry &entry = validEntries[i];
        chains << exportAudioFilterChainForEntry(
            i,
            entry,
            plan.channelModes.value(i, AudioChannelMode::Stereo),
            plan.reversed.value(i, false));
        if (renderInPlace) {
            // adelay can leave unset timestamps on its generated silence.
            // amix propagates them; atrim then cuts at the first real PTS.
            // Count samples after delay so both silence and content are 0-origin.
            chains << QStringLiteral("[a%1]asetpts=N/SR/TB[rip%1]").arg(i);
            mixInputs << QStringLiteral("[rip%1]").arg(i);
        } else {
            mixInputs << QStringLiteral("[a%1]").arg(i);
        }
    }

    chains << QStringLiteral("%1amix=inputs=%2:normalize=0:duration=longest,"
                             "%4atrim=duration=%3,"
                             "asetpts=PTS-STARTPTS[aout]")
        .arg(mixInputs.join(QString()), QString::number(validEntries.size()),
             ffmpegNumber(durationSeconds),
             renderInPlace ? QStringLiteral("asetpts=N/SR/TB,apad,") : QString());

    args << QStringLiteral("-filter_complex") << chains.join(QStringLiteral(";"))
         << QStringLiteral("-map") << QStringLiteral("[aout]")
         << QStringLiteral("-vn");
    return finishMix(args);
}

QString audioexport::measureAndConvert(const MixPlan &plan, const QString &mixPath, RenderJob &job,
                                       QString *error)
{
    QString localError;
    if (!error) error = &localError;
    // The measurement WAV is removed below; registering it makes removeTempFiles retry if that fails.
    if (!mixPath.isEmpty()) RenderQueue::addTempFile(job.exportConfig, mixPath);
    QString measurementPath = mixPath;
    QTemporaryDir rangeTemporary;
    if (!mixPath.isEmpty() && plan.trimMeasurement) {
        measurementPath = rangeTemporary.filePath(QStringLiteral("range.wav"));
        const QStringList args{QStringLiteral("-y"), QStringLiteral("-i"), mixPath,
            QStringLiteral("-ss"), ffmpegNumber(plan.measureStartSec), QStringLiteral("-t"),
            ffmpegNumber(plan.measureEndSec - plan.measureStartSec),
            QStringLiteral("-vn"), QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), measurementPath};
        if (!rangeTemporary.isValid() || !runFfmpegForAudioMix(args, error)) measurementPath.clear();
    }
    QString message = RenderQueue::measureAutoLoudness(job.exportConfig, measurementPath);
    if (!error->isEmpty()) {
        message += QStringLiteral(" / 警告: ") + *error;
        job.exportConfig["loudnessAutoStatus"] = message;
        qWarning().noquote() << message;
    }
    if (!mixPath.isEmpty()) {
        // The existing queue muxes compressed audio by stream copy. Use the
        // same AAC intermediate as the ordinary timeline mix after measuring PCM.
        const QString muxPath = nextExportAudioMixPath();
        QStringList args{QStringLiteral("-y"), QStringLiteral("-i"), mixPath, QStringLiteral("-vn")};
        args << exportaudio::aacAudioArgs(job.exportConfig.value("audioBitrate").toInt()) << muxPath;
        RenderQueue::addTempFile(job.exportConfig, muxPath);
        bool converted = runFfmpegForAudioMix(args, error);
        if (converted && plan.failAacConvertForTest) {
            converted = false;
            *error = QStringLiteral("test: forced AAC conversion failure");
        }
        if (converted) RenderQueue::setIntermediateProjectFile(job, muxPath);
        else {
            message += QStringLiteral(" / 音声ミックス変換失敗: ") + *error;
            job.exportConfig["loudnessAutoStatus"] = message;
            job.exportConfig["loudnessAutoError"] = *error;
            qWarning().noquote() << message;
        }
        QFile::remove(mixPath);
    }
    return message;
}

void audioexport::processTrackEntries(std::vector<DspEntry> &entries,
                                      const trackfx::Chain &chain,
                                      const std::array<AudioMixer::EqBandCoefs, 3> &coeffs,
                                      bool eqEnabled, double preampDb)
{
    // Sort indices so callers retain their clip-to-buffer mapping.
    std::vector<size_t> order;
    for (size_t i = 0; i < entries.size(); ++i)
        if (!entries[i].samples.empty()) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return entries[a].frameStart < entries[b].frameStart;
    });
    if (order.empty()) return;
    trackfx::Processor processor(chain, AudioMixer::kSampleRateHz, AudioMixer::kChannels);
    processor.reset();
    std::array<std::array<double, 4>, 3> z{};
    const double preamp = std::pow(10.0, preampDb / 20.0);
    qint64 end = entries[order.front()].frameStart;
    for (size_t i : order)
        end = std::max(end, entries[i].frameStart + static_cast<qint64>(entries[i].samples.size() / 2));
    for (qint64 t = entries[order.front()].frameStart; t < end;) {
        const qint64 blockEnd = std::min(t + 1024, end);
        qint64 next = end;
        bool active = false;
        for (size_t i : order) {
            auto &entry = entries[i];
            const qint64 first = std::max(t, entry.frameStart);
            const qint64 last = std::min(blockEnd,
                entry.frameStart + static_cast<qint64>(entry.samples.size() / 2));
            if (entry.frameStart >= blockEnd) next = std::min(next, entry.frameStart);
            if (first >= last) continue;
            active = true;
            float *block = entry.samples.data() + 2 * static_cast<size_t>(first - entry.frameStart);
            const int count = static_cast<int>(last - first);
            processor.process(block, count);
            if (!eqEnabled) continue;
            // Same double cascade as MixerIODevice::readData. In particular,
            // never saturate/quantize between the legacy EQ and preamp/gain.
            for (int sample = 0; sample < count * 2; ++sample) {
                double value = block[sample];
                const int ch = sample & 1;
                for (int b = 0; b < 3; ++b) {
                    const auto &c = coeffs[b];
                    const double w = value - c.a1 * z[b][ch] - c.a2 * z[b][ch + 2];
                    value = c.b0 * w + c.b1 * z[b][ch] + c.b2 * z[b][ch + 2];
                    z[b][ch + 2] = z[b][ch];
                    z[b][ch] = w;
                }
                block[sample] = static_cast<float>(value * preamp);
            }
        }
        // Gaps do not advance DSP histories or feed silence to NR/reverb.
        t = active ? blockEnd : next;
    }
}
