#include "PitchShifter.h"
#include "Timeline.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHash>
#include <QLabel>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace pitchshift {

namespace {

constexpr const char *kSettingsGroup = "PitchShift";
constexpr double kPi = 3.14159265358979323846;

QMutex g_mutex;
QHash<QString, PitchShiftParams> g_cache;
std::atomic<bool> g_loaded{false};
std::atomic<bool> g_anyActive{false};

void recomputeActiveLocked()
{
    bool any = false;
    for (auto it = g_cache.constBegin(); it != g_cache.constEnd(); ++it) {
        if (it.value().enabled) {
            any = true;
            break;
        }
    }
    g_anyActive.store(any, std::memory_order_release);
}

void ensureLoadedLocked()
{
    if (g_loaded.load(std::memory_order_acquire))
        return;
    g_cache.clear();
    QSettings s;
    s.beginGroup(QLatin1String(kSettingsGroup));
    for (const QString &uid : s.childGroups()) {
        s.beginGroup(uid);
        PitchShiftParams p;
        p.enabled = s.value(QStringLiteral("en"), false).toBool();
        p.semitones = s.value(QStringLiteral("st"), 0.0).toDouble();
        p.preset = static_cast<VoicePreset>(qBound(0, s.value(QStringLiteral("pr"), 0).toInt(), 6));
        p.wetMix = s.value(QStringLiteral("wet"), 1.0).toDouble();
        p.robotFreqHz = s.value(QStringLiteral("rf"), 85.0).toDouble();
        s.endGroup();
        if (p.enabled)
            g_cache.insert(uid, p);
    }
    s.endGroup();
    recomputeActiveLocked();
    g_loaded.store(true, std::memory_order_release);
}

// WSOLA-inspired overlap-add time-scaler: stretches `in` by `stretchFactor`
// without changing pitch, searching a small window around the nominal analysis
// offset to maximize waveform phase alignment (minimizing comb/chorus artifacts).
QVector<float> timeStretchWsola(const QVector<float> &in, double stretchFactor, int sampleRate)
{
    const int inLen = in.size();
    if (inLen < 64 || std::abs(stretchFactor - 1.0) < 1e-4)
        return in;

    const int outLen = qMax(1, static_cast<int>(std::lround(inLen * stretchFactor)));
    const int winSize = qBound(256, (sampleRate / 35) & ~1, 2048); // ~28ms Hann window
    const int synHop = winSize / 2;
    const double anaHopF = static_cast<double>(synHop) / stretchFactor;
    const int searchRad = qMin(synHop / 2, 160);

    QVector<float> hann(winSize);
    for (int i = 0; i < winSize; ++i)
        hann[i] = 0.5f * (1.0f - std::cos(static_cast<float>(2.0 * kPi * i / winSize)));

    QVector<float> out(outLen, 0.0f);
    QVector<float> norm(outLen, 0.0f);

    int prevAnaPos = 0;
    int outPos = 0;
    int frameIdx = 0;

    while (outPos < outLen) {
        const int nominalAna = static_cast<int>(std::lround(frameIdx * anaHopF));
        int bestAna = qBound(0, nominalAna, qMax(0, inLen - winSize));

        if (frameIdx > 0 && inLen > winSize + 4) {
            const int expectedCont = prevAnaPos + synHop;
            if (expectedCont + synHop <= inLen) {
                const int lo = qMax(0, nominalAna - searchRad);
                const int hi = qMin(inLen - winSize, nominalAna + searchRad);
                double bestCorr = -1e30;
                for (int cand = lo; cand <= hi; cand += 2) {
                    double corr = 0.0;
                    for (int k = 0; k < synHop; k += 2)
                        corr += static_cast<double>(in[cand + k]) * static_cast<double>(in[expectedCont + k]);
                    if (corr > bestCorr) {
                        bestCorr = corr;
                        bestAna = cand;
                    }
                }
            }
        }

        for (int i = 0; i < winSize && (outPos + i) < outLen; ++i) {
            const int srcIdx = bestAna + i;
            if (srcIdx >= 0 && srcIdx < inLen) {
                const float w = hann[i];
                out[outPos + i] += in[srcIdx] * w;
                norm[outPos + i] += w;
            }
        }

        prevAnaPos = bestAna;
        outPos += synHop;
        ++frameIdx;
    }

    for (int i = 0; i < outLen; ++i) {
        if (norm[i] > 1e-4f)
            out[i] /= norm[i];
    }
    return out;
}

QVector<float> resampleLinearToLength(const QVector<float> &in, int targetLen)
{
    QVector<float> out(targetLen, 0.0f);
    if (in.isEmpty() || targetLen <= 0)
        return out;
    if (in.size() == 1 || targetLen == 1) {
        out.fill(in.first());
        return out;
    }
    const double step = static_cast<double>(in.size() - 1) / static_cast<double>(targetLen - 1);
    for (int i = 0; i < targetLen; ++i) {
        const double pos = i * step;
        const int idx = qBound(0, static_cast<int>(pos), in.size() - 2);
        const float frac = static_cast<float>(pos - idx);
        out[i] = in[idx] * (1.0f - frac) + in[idx + 1] * frac;
    }
    return out;
}

void applyTelephoneBandpass(QVector<float> &buf, int sampleRate)
{
    if (buf.isEmpty() || sampleRate <= 0)
        return;
    // Simple 1st-order HPF @ 320 Hz + 1st-order LPF @ 3200 Hz + mild soft clipping
    const double dt = 1.0 / sampleRate;
    const double rcHp = 1.0 / (2.0 * kPi * 320.0);
    const double aHp = rcHp / (rcHp + dt);
    const double rcLp = 1.0 / (2.0 * kPi * 3200.0);
    const double aLp = dt / (rcLp + dt);

    double prevIn = 0.0;
    double prevHp = 0.0;
    double prevLp = 0.0;
    for (int i = 0; i < buf.size(); ++i) {
        const double x = buf[i];
        const double hp = aHp * (prevHp + x - prevIn);
        prevIn = x;
        prevHp = hp;
        const double lp = prevLp + aLp * (hp - prevLp);
        prevLp = lp;
        buf[i] = static_cast<float>(std::tanh(lp * 1.6));
    }
}

quint16 rd16(const uchar *p) { return static_cast<quint16>(p[0] | (p[1] << 8)); }
quint32 rd32(const uchar *p)
{
    return static_cast<quint32>(p[0]) | (static_cast<quint32>(p[1]) << 8)
         | (static_cast<quint32>(p[2]) << 16) | (static_cast<quint32>(p[3]) << 24);
}
void wr16(QFile &f, quint16 v) { f.write(reinterpret_cast<const char *>(&v), 2); }
void wr32(QFile &f, quint32 v) { f.write(reinterpret_cast<const char *>(&v), 4); }

} // namespace

QString presetDisplayName(VoicePreset preset)
{
    switch (preset) {
    case VoicePreset::Natural:        return QStringLiteral("カスタム / ナチュラル");
    case VoicePreset::Chipmunk:       return QStringLiteral("チップマンク (+6 半音)");
    case VoicePreset::DeepVoice:      return QStringLiteral("ディープボイス (-5 半音)");
    case VoicePreset::Robot:          return QStringLiteral("ロボット (リング変調)");
    case VoicePreset::TelephoneRadio: return QStringLiteral("電話 / ラジオ風");
    case VoicePreset::Helium:         return QStringLiteral("ヘリウム (+9 半音)");
    case VoicePreset::Demon:          return QStringLiteral("デーモン (-8 半音)");
    }
    return QStringLiteral("カスタム");
}

PitchShiftParams paramsForPreset(VoicePreset preset)
{
    PitchShiftParams p;
    p.enabled = true;
    p.preset = preset;
    p.wetMix = 1.0;
    switch (preset) {
    case VoicePreset::Natural:        p.semitones = 0.0; break;
    case VoicePreset::Chipmunk:       p.semitones = 6.0; break;
    case VoicePreset::DeepVoice:      p.semitones = -5.0; break;
    case VoicePreset::Robot:          p.semitones = 0.0; p.robotFreqHz = 85.0; break;
    case VoicePreset::TelephoneRadio: p.semitones = 0.0; break;
    case VoicePreset::Helium:         p.semitones = 9.0; break;
    case VoicePreset::Demon:          p.semitones = -8.0; break;
    }
    return p;
}

bool hasAnyActiveConfig()
{
    if (!g_loaded.load(std::memory_order_acquire)) {
        QMutexLocker lock(&g_mutex);
        ensureLoadedLocked();
    }
    return g_anyActive.load(std::memory_order_acquire);
}

PitchShiftParams configForClip(const QString &clipUid)
{
    if (clipUid.isEmpty())
        return {};
    QMutexLocker lock(&g_mutex);
    ensureLoadedLocked();
    return g_cache.value(clipUid);
}

void setConfigForClip(const QString &clipUid, const PitchShiftParams &params)
{
    if (clipUid.isEmpty())
        return;
    QMutexLocker lock(&g_mutex);
    ensureLoadedLocked();

    QSettings s;
    s.beginGroup(QLatin1String(kSettingsGroup));
    if (!params.enabled) {
        g_cache.remove(clipUid);
        s.remove(clipUid);
    } else {
        g_cache.insert(clipUid, params);
        s.beginGroup(clipUid);
        s.setValue(QStringLiteral("en"), params.enabled);
        s.setValue(QStringLiteral("st"), params.semitones);
        s.setValue(QStringLiteral("pr"), static_cast<int>(params.preset));
        s.setValue(QStringLiteral("wet"), params.wetMix);
        s.setValue(QStringLiteral("rf"), params.robotFreqHz);
        s.endGroup();
    }
    s.endGroup();
    s.sync();
    recomputeActiveLocked();
}

void clearConfigForClip(const QString &clipUid)
{
    if (clipUid.isEmpty())
        return;
    setConfigForClip(clipUid, PitchShiftParams{});
}

void clearAllConfigsForTest()
{
    QMutexLocker lock(&g_mutex);
    g_cache.clear();
    QSettings s;
    s.remove(QLatin1String(kSettingsGroup));
    s.sync();
    g_anyActive.store(false, std::memory_order_release);
    g_loaded.store(true, std::memory_order_release);
}

QVector<float> processMonoBuffer(const QVector<float> &input,
                                 int sampleRate,
                                 const PitchShiftParams &params)
{
    if (input.isEmpty() || !params.enabled || sampleRate <= 0)
        return input;

    const double clampedSt = qBound(-12.0, params.semitones, 12.0);
    const double pitchRatio = std::pow(2.0, clampedSt / 12.0);

    QVector<float> wet;
    if (std::abs(clampedSt) > 1e-3) {
        // Stretch by pitchRatio first, then resample back to original length ->
        // frequency multiplies by pitchRatio while duration stays identical!
        const QVector<float> stretched = timeStretchWsola(input, pitchRatio, sampleRate);
        wet = resampleLinearToLength(stretched, input.size());
    } else {
        wet = input;
    }

    if (params.preset == VoicePreset::Robot) {
        const double freq = qBound(20.0, params.robotFreqHz, 400.0);
        for (int i = 0; i < wet.size(); ++i) {
            const double mod = std::sin(2.0 * kPi * freq * i / sampleRate);
            wet[i] = static_cast<float>(wet[i] * mod);
        }
    } else if (params.preset == VoicePreset::TelephoneRadio) {
        applyTelephoneBandpass(wet, sampleRate);
    }

    const float mix = static_cast<float>(qBound(0.0, params.wetMix, 1.0));
    if (mix >= 0.999f)
        return wet;

    QVector<float> out(input.size());
    for (int i = 0; i < input.size(); ++i)
        out[i] = input[i] * (1.0f - mix) + wet[i] * mix;
    return out;
}

double estimateDominantFrequencyHz(const QVector<float> &mono, int sampleRate)
{
    if (mono.size() < 256 || sampleRate <= 0)
        return 0.0;
    // Analyze middle 2048 samples with Hann window via Goertzel/DFT over 60..2400 Hz
    const int N = qMin(2048, mono.size());
    const int offset = (mono.size() - N) / 2;
    QVector<double> win(N);
    for (int i = 0; i < N; ++i)
        win[i] = mono[offset + i] * 0.5 * (1.0 - std::cos(2.0 * kPi * i / N));

    const int minBin = qMax(1, static_cast<int>(std::floor(60.0 * N / sampleRate)));
    const int maxBin = qMin(N / 2 - 1, static_cast<int>(std::ceil(2400.0 * N / sampleRate)));

    double bestMag = -1.0;
    int bestBin = minBin;
    for (int k = minBin; k <= maxBin; ++k) {
        const double w = 2.0 * kPi * k / N;
        double re = 0.0;
        double im = 0.0;
        for (int n = 0; n < N; ++n) {
            const double ang = w * n;
            re += win[n] * std::cos(ang);
            im -= win[n] * std::sin(ang);
        }
        const double mag = re * re + im * im;
        if (mag > bestMag) {
            bestMag = mag;
            bestBin = k;
        }
    }
    return static_cast<double>(bestBin) * static_cast<double>(sampleRate) / static_cast<double>(N);
}

bool processWavFile(const QString &inputWavPath,
                    const QString &outputWavPath,
                    const PitchShiftParams &params,
                    QString *errorOut)
{
    QFile inFile(inputWavPath);
    if (!inFile.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("入力 WAV を開けません: %1").arg(inputWavPath);
        return false;
    }
    const QByteArray raw = inFile.readAll();
    if (raw.size() < 44 || std::memcmp(raw.constData(), "RIFF", 4) != 0
        || std::memcmp(raw.constData() + 8, "WAVE", 4) != 0) {
        if (errorOut) *errorOut = QStringLiteral("16-bit PCM WAV 形式ではありません。");
        return false;
    }

    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
    const uchar *pcmData = nullptr;
    quint32 pcmBytes = 0;

    int pos = 12;
    while (pos + 8 <= raw.size()) {
        const uchar *hdr = reinterpret_cast<const uchar *>(raw.constData() + pos);
        const quint32 sz = rd32(hdr + 4);
        pos += 8;
        if (pos + static_cast<int>(sz) > raw.size())
            break;
        if (std::memcmp(hdr, "fmt ", 4) == 0 && sz >= 16) {
            const quint16 fmtTag = rd16(hdr + 8);
            channels = rd16(hdr + 10);
            sampleRate = static_cast<int>(rd32(hdr + 12));
            bitsPerSample = rd16(hdr + 22);
            if (fmtTag != 1 || bitsPerSample != 16 || channels < 1 || channels > 2) {
                if (errorOut) *errorOut = QStringLiteral("16-bit PCM (mono/stereo) WAV のみ対応しています。");
                return false;
            }
        } else if (std::memcmp(hdr, "data", 4) == 0) {
            pcmData = hdr + 8;
            pcmBytes = sz;
        }
        pos += static_cast<int>(sz) + (sz & 1u);
    }

    if (!pcmData || pcmBytes == 0 || sampleRate <= 0 || channels <= 0) {
        if (errorOut) *errorOut = QStringLiteral("WAV データチャンクが見つかりません。");
        return false;
    }

    const int totalFrames = static_cast<int>(pcmBytes / (channels * 2));
    const auto *src16 = reinterpret_cast<const qint16 *>(pcmData);

    QVector<QVector<float>> chBufs(channels);
    for (int ch = 0; ch < channels; ++ch) {
        chBufs[ch].resize(totalFrames);
        for (int i = 0; i < totalFrames; ++i)
            chBufs[ch][i] = static_cast<float>(src16[i * channels + ch]) / 32768.0f;
        chBufs[ch] = processMonoBuffer(chBufs[ch], sampleRate, params);
    }

    QDir().mkpath(QFileInfo(outputWavPath).absolutePath());
    QFile outFile(outputWavPath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("出力 WAV を作成できません: %1").arg(outputWavPath);
        return false;
    }

    const quint32 dataBytes = static_cast<quint32>(totalFrames * channels * 2);
    outFile.write("RIFF", 4);
    wr32(outFile, 36 + dataBytes);
    outFile.write("WAVE", 4);
    outFile.write("fmt ", 4);
    wr32(outFile, 16);
    wr16(outFile, 1);
    wr16(outFile, static_cast<quint16>(channels));
    wr32(outFile, static_cast<quint32>(sampleRate));
    wr32(outFile, static_cast<quint32>(sampleRate * channels * 2));
    wr16(outFile, static_cast<quint16>(channels * 2));
    wr16(outFile, 16);
    outFile.write("data", 4);
    wr32(outFile, dataBytes);

    QByteArray outPcm(static_cast<int>(dataBytes), '\0');
    auto *dst16 = reinterpret_cast<qint16 *>(outPcm.data());
    for (int i = 0; i < totalFrames; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            const float clamped = qBound(-1.0f, chBufs[ch][i], 1.0f);
            dst16[i * channels + ch] = static_cast<qint16>(std::lround(clamped * 32767.0f));
        }
    }
    return outFile.write(outPcm) == outPcm.size();
}

bool applyPitchShiftToAudioClip(Timeline *timeline,
                                int audioTrackIdx,
                                int clipIdx,
                                const PitchShiftParams &params,
                                const QString &outputWavPath,
                                QString *errorOut)
{
    if (!timeline) {
        if (errorOut) *errorOut = QStringLiteral("タイムラインが無効です。");
        return false;
    }
    const auto tracks = timeline->audioTracks();
    if (audioTrackIdx < 0 || audioTrackIdx >= tracks.size() || !tracks[audioTrackIdx]) {
        if (errorOut) *errorOut = QStringLiteral("オーディオトラックが見つかりません。");
        return false;
    }
    TimelineTrack *tr = tracks[audioTrackIdx];
    if (tr->isLocked()) {
        if (errorOut) *errorOut = QStringLiteral("トラックがロックされています。");
        return false;
    }
    QVector<ClipInfo> clips = tr->clips();
    if (clipIdx < 0 || clipIdx >= clips.size()) {
        if (errorOut) *errorOut = QStringLiteral("クリップが見つかりません。");
        return false;
    }

    if (!processWavFile(clips[clipIdx].filePath, outputWavPath, params, errorOut))
        return false;

    clips[clipIdx].filePath = outputWavPath;
    if (!clips[clipIdx].uid.isEmpty())
        setConfigForClip(clips[clipIdx].uid, params);
    tr->setClips(clips);
    timeline->saveUndoState(QStringLiteral("ピッチシフト / ボイスチェンジャーを適用 (%1)")
                                .arg(presetDisplayName(params.preset)));
    return true;
}

// ============================================================================
// PitchShiftDialog
// ============================================================================

PitchShiftDialog::PitchShiftDialog(const QString &clipUid,
                                   const QString &clipLabel,
                                   QWidget *parent)
    : QDialog(parent)
    , m_clipUid(clipUid)
{
    setWindowTitle(QStringLiteral("ピッチシフト / ボイスチェンジャー"));
    setMinimumWidth(400);

    auto *root = new QVBoxLayout(this);
    auto *hdr = new QLabel(QStringLiteral("対象クリップ: %1").arg(clipLabel), this);
    hdr->setStyleSheet(QStringLiteral("font-weight: bold;"));
    root->addWidget(hdr);

    auto *form = new QFormLayout();
    m_enableCheck = new QCheckBox(QStringLiteral("ピッチシフト / ボイスチェンジャーを有効にする"), this);
    form->addRow(m_enableCheck);

    m_presetCombo = new QComboBox(this);
    for (int i = 0; i <= static_cast<int>(VoicePreset::Demon); ++i)
        m_presetCombo->addItem(presetDisplayName(static_cast<VoicePreset>(i)), i);
    form->addRow(QStringLiteral("プリセット:"), m_presetCombo);

    m_semitonesSpin = new QDoubleSpinBox(this);
    m_semitonesSpin->setRange(-12.0, 12.0);
    m_semitonesSpin->setSingleStep(0.5);
    m_semitonesSpin->setDecimals(1);
    m_semitonesSpin->setSuffix(QStringLiteral(" st"));
    form->addRow(QStringLiteral("ピッチシフト (半音):"), m_semitonesSpin);

    m_wetMixSpin = new QDoubleSpinBox(this);
    m_wetMixSpin->setRange(0.0, 1.0);
    m_wetMixSpin->setSingleStep(0.1);
    m_wetMixSpin->setDecimals(2);
    form->addRow(QStringLiteral("適用量 (Wet Mix):"), m_wetMixSpin);

    m_robotFreqSpin = new QDoubleSpinBox(this);
    m_robotFreqSpin->setRange(20.0, 400.0);
    m_robotFreqSpin->setSuffix(QStringLiteral(" Hz"));
    form->addRow(QStringLiteral("ロボット変調周波数:"), m_robotFreqSpin);
    root->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &PitchShiftDialog::onAcceptSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_presetCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &PitchShiftDialog::onPresetChanged);

    applyFromParams(configForClip(m_clipUid));
}

PitchShiftParams PitchShiftDialog::currentParams() const
{
    PitchShiftParams p;
    p.enabled = m_enableCheck->isChecked();
    p.preset = static_cast<VoicePreset>(m_presetCombo->currentData().toInt());
    p.semitones = m_semitonesSpin->value();
    p.wetMix = m_wetMixSpin->value();
    p.robotFreqHz = m_robotFreqSpin->value();
    return p;
}

void PitchShiftDialog::onPresetChanged(int idx)
{
    if (idx <= 0)
        return;
    const PitchShiftParams preset = paramsForPreset(static_cast<VoicePreset>(idx));
    m_enableCheck->setChecked(true);
    m_semitonesSpin->setValue(preset.semitones);
    m_wetMixSpin->setValue(preset.wetMix);
    m_robotFreqSpin->setValue(preset.robotFreqHz);
}

void PitchShiftDialog::applyFromParams(const PitchShiftParams &p)
{
    m_presetCombo->blockSignals(true);
    m_enableCheck->setChecked(p.enabled);
    m_presetCombo->setCurrentIndex(static_cast<int>(p.preset));
    m_semitonesSpin->setValue(p.semitones);
    m_wetMixSpin->setValue(p.wetMix);
    m_robotFreqSpin->setValue(p.robotFreqHz);
    m_presetCombo->blockSignals(false);
}

void PitchShiftDialog::onAcceptSave()
{
    if (!m_clipUid.isEmpty())
        setConfigForClip(m_clipUid, currentParams());
    accept();
}

} // namespace pitchshift
