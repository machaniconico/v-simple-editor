#include "AudioEQ.h"
#include <QThread>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QFileInfo>
#include <cmath>

// --- AudioEffect helpers ---

QString AudioEffect::typeName(AudioEffectType t)
{
    switch (t) {
    case AudioEffectType::EQ:           return "EQ";
    case AudioEffectType::Reverb:       return "Reverb";
    case AudioEffectType::Compressor:   return "Compressor";
    case AudioEffectType::Normalize:    return "Normalize";
    case AudioEffectType::FadeIn:       return "Fade In";
    case AudioEffectType::FadeOut:      return "Fade Out";
    case AudioEffectType::BassBoost:    return "Bass Boost";
    case AudioEffectType::VoiceEnhance: return "Voice Enhance";
    }
    return "Unknown";
}

QVector<AudioEffectType> AudioEffect::allTypes()
{
    return {
        AudioEffectType::EQ,
        AudioEffectType::Reverb,
        AudioEffectType::Compressor,
        AudioEffectType::Normalize,
        AudioEffectType::FadeIn,
        AudioEffectType::FadeOut,
        AudioEffectType::BassBoost,
        AudioEffectType::VoiceEnhance
    };
}

AudioEffect AudioEffect::createReverb(double roomSize, double damping, double wetLevel)
{
    AudioEffect e;
    e.type = AudioEffectType::Reverb;
    e.p1 = roomSize;
    e.p2 = damping;
    e.p3 = wetLevel;
    return e;
}

AudioEffect AudioEffect::createCompressor(double threshold, double ratio,
                                           double attack, double release)
{
    AudioEffect e;
    e.type = AudioEffectType::Compressor;
    e.p1 = threshold;
    e.p2 = ratio;
    e.p3 = attack;
    e.p4 = release;
    return e;
}

AudioEffect AudioEffect::createNormalize(double targetLevel)
{
    AudioEffect e;
    e.type = AudioEffectType::Normalize;
    e.p1 = targetLevel;
    return e;
}

AudioEffect AudioEffect::createFadeIn(double duration)
{
    AudioEffect e;
    e.type = AudioEffectType::FadeIn;
    e.p1 = duration;
    return e;
}

AudioEffect AudioEffect::createFadeOut(double duration)
{
    AudioEffect e;
    e.type = AudioEffectType::FadeOut;
    e.p1 = duration;
    return e;
}

AudioEffect AudioEffect::createBassBoost(double gain, double frequency)
{
    AudioEffect e;
    e.type = AudioEffectType::BassBoost;
    e.p1 = gain;
    e.p2 = frequency;
    return e;
}

AudioEffect AudioEffect::createVoiceEnhance(double clarity)
{
    AudioEffect e;
    e.type = AudioEffectType::VoiceEnhance;
    e.p1 = clarity;
    return e;
}

// --- AudioEQProcessor ---

AudioEQProcessor::AudioEQProcessor(QObject *parent)
    : QObject(parent)
{
}

void AudioEQProcessor::cancel()
{
    m_cancelled = true;
}

// --- FFmpeg binary lookup ---

QString AudioEQProcessor::findFFmpegBinary()
{
    QString path = QStandardPaths::findExecutable("ffmpeg");
    if (!path.isEmpty())
        return path;

    // Common macOS / Linux locations
    QStringList searchPaths = {"/usr/local/bin", "/opt/homebrew/bin", "/usr/bin"};
    path = QStandardPaths::findExecutable("ffmpeg", searchPaths);
    return path;
}

// --- Progress parsing from FFmpeg stderr ---

void AudioEQProcessor::parseProgress(const QString &output)
{
    // Parse total duration: "Duration: HH:MM:SS.ms"
    if (m_totalDuration <= 0.0) {
        static QRegularExpression durRe(R"(Duration:\s+(\d+):(\d+):(\d+)\.(\d+))");
        auto match = durRe.match(output);
        if (match.hasMatch()) {
            m_totalDuration = match.captured(1).toDouble() * 3600.0
                            + match.captured(2).toDouble() * 60.0
                            + match.captured(3).toDouble()
                            + match.captured(4).toDouble() / 100.0;
        }
    }

    // Parse current position: "time=HH:MM:SS.ms"
    if (m_totalDuration > 0.0) {
        static QRegularExpression timeRe(R"(time=(\d+):(\d+):(\d+)\.(\d+))");
        auto match = timeRe.match(output);
        if (match.hasMatch()) {
            double currentTime = match.captured(1).toDouble() * 3600.0
                               + match.captured(2).toDouble() * 60.0
                               + match.captured(3).toDouble()
                               + match.captured(4).toDouble() / 100.0;
            double ratio = qBound(0.0, currentTime / m_totalDuration, 1.0);
            int pct = static_cast<int>(ratio * 100.0);
            emit progressChanged(qBound(0, pct, 100));
        }
    }
}

// --- Run FFmpeg process synchronously (called from worker thread) ---

bool AudioEQProcessor::runFFmpeg(const QStringList &args)
{
    QString ffmpegBin = findFFmpegBinary();
    if (ffmpegBin.isEmpty())
        return false;

    m_process = new QProcess();

    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        QString output = m_process->readAllStandardError();
        parseProgress(output);
    });

    m_process->start(ffmpegBin, args);
    m_process->waitForStarted();

    // Poll for completion or cancellation
    while (!m_process->waitForFinished(200)) {
        if (m_cancelled) {
            m_process->kill();
            m_process->waitForFinished(3000);
            m_process->deleteLater();
            m_process = nullptr;
            return false;
        }
    }

    bool success = (m_process->exitStatus() == QProcess::NormalExit
                    && m_process->exitCode() == 0);

    m_process->deleteLater();
    m_process = nullptr;
    return success;
}

// --- Filter builders ---

QString AudioEQProcessor::buildEQFilter(const AudioEQConfig &eqConfig)
{
    QStringList filters;

    // Pre-amplification via volume filter
    if (eqConfig.preamp != 0.0) {
        filters << QString("volume=%1dB").arg(eqConfig.preamp, 0, 'f', 1);
    }

    // Parametric EQ bands via FFmpeg equalizer filter
    for (const auto &band : eqConfig.bands) {
        if (band.isFlat()) continue;

        // equalizer=f=<freq>:t=q:w=<q>:g=<gain>
        filters << QString("equalizer=f=%1:t=q:w=%2:g=%3")
                       .arg(band.frequency, 0, 'f', 1)
                       .arg(band.q, 0, 'f', 2)
                       .arg(band.gain, 0, 'f', 1);
    }

    return filters.join(",");
}

QString AudioEQProcessor::buildEffectFilter(const AudioEffect &effect)
{
    if (!effect.enabled) return {};

    switch (effect.type) {
    case AudioEffectType::EQ:
        // EQ is handled by buildEQFilter
        return {};

    case AudioEffectType::Reverb: {
        // aecho: simulate reverb via delays
        //   in_gain|out_gain|delays|decays
        // Map roomSize to delay (20-100ms), damping to decay (0.1-0.7),
        // wetLevel to output gain
        double delay = 20.0 + effect.p1 * 80.0;     // roomSize -> delay ms
        double decay = 0.1 + (1.0 - effect.p2) * 0.6; // damping -> decay
        double outGain = effect.p3;                   // wetLevel
        double inGain = 0.8;
        return QString("aecho=%1:%2:%3:%4")
                   .arg(inGain, 0, 'f', 2)
                   .arg(outGain, 0, 'f', 2)
                   .arg(delay, 0, 'f', 0)
                   .arg(decay, 0, 'f', 2);
    }

    case AudioEffectType::Compressor: {
        // acompressor: threshold, ratio, attack, release
        // threshold in dB (negative), ratio as N:1, attack/release in ms
        return QString("acompressor=threshold=%1dB:ratio=%2:attack=%3:release=%4")
                   .arg(effect.p1, 0, 'f', 1)
                   .arg(effect.p2, 0, 'f', 1)
                   .arg(effect.p3, 0, 'f', 0)
                   .arg(effect.p4, 0, 'f', 0);
    }

    case AudioEffectType::Normalize: {
        // loudnorm: EBU R128 loudness normalization
        //   I = integrated loudness target (LUFS, maps from dB target)
        double targetLUFS = -24.0 + effect.p1; // map -20..0 to -44..-24 range
        return QString("loudnorm=I=%1:TP=-1.5:LRA=11")
                   .arg(targetLUFS, 0, 'f', 1);
    }

    case AudioEffectType::FadeIn: {
        // afade: type=in, duration in seconds
        return QString("afade=t=in:d=%1").arg(effect.p1, 0, 'f', 2);
    }

    case AudioEffectType::FadeOut: {
        // afade: type=out, start_time computed from stream end (placeholder; actual
        // start will be computed at runtime). Use st=0 as placeholder.
        // In practice the caller should compute st = totalDuration - p1
        return QString("afade=t=out:d=%1").arg(effect.p1, 0, 'f', 2);
    }

    case AudioEffectType::BassBoost: {
        // Bass boost via equalizer: low shelf at specified frequency with gain
        return QString("equalizer=f=%1:t=q:w=0.7:g=%2")
                   .arg(effect.p2, 0, 'f', 1)
                   .arg(effect.p1, 0, 'f', 1);
    }

    case AudioEffectType::VoiceEnhance: {
        // Voice enhancement: boost presence (2-4kHz), cut rumble (<100Hz)
        //   clarity (0-1) controls the boost amount (0-12dB)
        double boostDb = effect.p1 * 12.0;
        QStringList parts;
        // Cut low rumble
        parts << "highpass=f=80";
        // Boost presence range
        parts << QString("equalizer=f=3000:t=q:w=1.5:g=%1").arg(boostDb, 0, 'f', 1);
        // Slight de-ess at 6-8kHz
        parts << QString("equalizer=f=7000:t=q:w=2.0:g=%1").arg(-boostDb * 0.3, 0, 'f', 1);
        return parts.join(",");
    }
    }

    return {};
}

QString AudioEQProcessor::buildFilterString(const AudioEQConfig &eqConfig,
                                            const QVector<AudioEffect> &effects)
{
    QStringList filterParts;

    // EQ portion
    QString eqPart = buildEQFilter(eqConfig);
    if (!eqPart.isEmpty())
        filterParts << eqPart;

    // Effect chain
    for (const auto &effect : effects) {
        QString part = buildEffectFilter(effect);
        if (!part.isEmpty())
            filterParts << part;
    }

    return filterParts.join(",");
}

// --- Built-in presets ---

QVector<EQPreset> AudioEQProcessor::presets()
{
    QVector<EQPreset> list;

    // Flat — all bands at 0
    {
        EQPreset p;
        p.name = "Flat";
        p.config.bands = {
            {60, 0, 1.0}, {250, 0, 1.0}, {1000, 0, 1.0},
            {4000, 0, 1.0}, {12000, 0, 1.0}
        };
        list << p;
    }

    // Voice Clarity — boost 2-4kHz, cut below 100Hz
    {
        EQPreset p;
        p.name = "Voice Clarity";
        p.config.bands = {
            {60, -6, 1.0}, {250, -2, 1.0}, {1000, 0, 1.0},
            {3000, 5, 1.5}, {5000, 3, 1.0}, {12000, 0, 1.0}
        };
        list << p;
    }

    // Bass Boost — boost 60-250Hz
    {
        EQPreset p;
        p.name = "Bass Boost";
        p.config.bands = {
            {60, 8, 0.8}, {150, 6, 1.0}, {400, 2, 1.0},
            {1000, 0, 1.0}, {4000, 0, 1.0}, {12000, 0, 1.0}
        };
        list << p;
    }

    // Podcast — cut lows, boost presence, light compression
    {
        EQPreset p;
        p.name = "Podcast";
        p.config.bands = {
            {60, -8, 1.0}, {200, -3, 1.0}, {1000, 0, 1.0},
            {3000, 4, 1.5}, {5000, 2, 1.0}, {10000, -2, 1.0}
        };
        list << p;
    }

    // Music — slight V-curve (boost lows and highs)
    {
        EQPreset p;
        p.name = "Music";
        p.config.bands = {
            {60, 4, 0.8}, {250, 2, 1.0}, {1000, -1, 1.0},
            {4000, 2, 1.0}, {12000, 4, 0.8}
        };
        list << p;
    }

    return list;
}

// --- Public API ---

void AudioEQProcessor::applyEQ(const QString &inputPath, const QString &outputPath,
                                const AudioEQConfig &eqConfig)
{
    applyChain(inputPath, outputPath, eqConfig, {});
}

void AudioEQProcessor::applyEffect(const QString &inputPath, const QString &outputPath,
                                    const AudioEffect &effect)
{
    applyChain(inputPath, outputPath, {}, {effect});
}

void AudioEQProcessor::applyChain(const QString &inputPath, const QString &outputPath,
                                   const AudioEQConfig &eqConfig,
                                   const QVector<AudioEffect> &effects)
{
    m_cancelled = false;
    m_totalDuration = 0.0;

    QThread *thread = QThread::create([this, inputPath, outputPath, eqConfig, effects]() {
        emit progressChanged(0);

        // Validate input
        if (!QFileInfo::exists(inputPath)) {
            emit processComplete(false, "Input file not found: " + inputPath);
            return;
        }

        // Build combined filter string
        QString filterStr = buildFilterString(eqConfig, effects);
        if (filterStr.isEmpty()) {
            emit processComplete(false, "No filters to apply");
            return;
        }

        // Build FFmpeg args: input -> audio filter -> output
        QStringList args;
        args << "-y"                    // overwrite output
             << "-i" << inputPath
             << "-af" << filterStr
             << "-vn"                   // no video (audio processing only)
             << outputPath;

        bool success = runFFmpeg(args);

        if (m_cancelled) {
            emit processComplete(false, "Processing cancelled");
        } else if (success) {
            emit progressChanged(100);
            emit processComplete(true, "Audio processing complete");
        } else {
            emit processComplete(false, "FFmpeg audio processing failed");
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}
