#include "CreativeEffects.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
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
#include <vector>

namespace creative {

namespace {

constexpr const char *kSettingsGroup = "CreativeEffects";

QMutex g_storeMutex;
QHash<QString, ClipCreativeConfig> g_cache;
std::atomic<bool> g_loaded{false};
std::atomic<bool> g_hasAnyActive{false};

void recomputeActiveFlagLocked()
{
    bool any = false;
    for (auto it = g_cache.constBegin(); it != g_cache.constEnd(); ++it) {
        if (it.value().isAnyEnabled()) {
            any = true;
            break;
        }
    }
    g_hasAnyActive.store(any, std::memory_order_release);
}

void ensureLoadedLocked()
{
    if (g_loaded.load(std::memory_order_acquire))
        return;

    g_cache.clear();
    QSettings settings;
    settings.beginGroup(QLatin1String(kSettingsGroup));
    const QStringList uids = settings.childGroups();
    for (const QString &uid : uids) {
        settings.beginGroup(uid);
        ClipCreativeConfig cfg;
        cfg.halation.enabled = settings.value(QStringLiteral("hal_en"), false).toBool();
        cfg.halation.threshold = settings.value(QStringLiteral("hal_thr"), 0.78).toDouble();
        cfg.halation.radiusPx = settings.value(QStringLiteral("hal_rad"), 12.0).toDouble();
        cfg.halation.strength = settings.value(QStringLiteral("hal_str"), 0.65).toDouble();

        cfg.posterizeTime.enabled = settings.value(QStringLiteral("pt_en"), false).toBool();
        cfg.posterizeTime.targetFps = settings.value(QStringLiteral("pt_fps"), 12.0).toDouble();

        cfg.cameraDof.enabled = settings.value(QStringLiteral("dof_en"), false).toBool();
        cfg.cameraDof.focusDistance = settings.value(QStringLiteral("dof_foc"), 1000.0).toDouble();
        cfg.cameraDof.aperture = settings.value(QStringLiteral("dof_ap"), 50.0).toDouble();
        cfg.cameraDof.maxBlurPx = settings.value(QStringLiteral("dof_max"), 18.0).toDouble();
        settings.endGroup();

        if (cfg.isAnyEnabled())
            g_cache.insert(uid, cfg);
    }
    settings.endGroup();

    recomputeActiveFlagLocked();
    g_loaded.store(true, std::memory_order_release);
}

// Separable 1D box blur pass on a single-channel float buffer (w x h).
void boxBlurScalar1D(const std::vector<float> &src, std::vector<float> &dst,
                     int w, int h, int radius, bool horizontal)
{
    if (radius <= 0) {
        dst = src;
        return;
    }
    const float invWindow = 1.0f / static_cast<float>(2 * radius + 1);
    if (horizontal) {
        for (int y = 0; y < h; ++y) {
            const float *row = src.data() + static_cast<size_t>(y) * w;
            float *outRow = dst.data() + static_cast<size_t>(y) * w;
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k)
                sum += row[qBound(0, k, w - 1)];
            outRow[0] = sum * invWindow;
            for (int x = 1; x < w; ++x) {
                const int addIdx = qMin(w - 1, x + radius);
                const int subIdx = qMax(0, x - radius - 1);
                sum += row[addIdx] - row[subIdx];
                outRow[x] = sum * invWindow;
            }
        }
    } else {
        for (int x = 0; x < w; ++x) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k)
                sum += src[static_cast<size_t>(qBound(0, k, h - 1)) * w + x];
            dst[x] = sum * invWindow;
            for (int y = 1; y < h; ++y) {
                const int addIdx = qMin(h - 1, y + radius);
                const int subIdx = qMax(0, y - radius - 1);
                sum += src[static_cast<size_t>(addIdx) * w + x]
                     - src[static_cast<size_t>(subIdx) * w + x];
                dst[static_cast<size_t>(y) * w + x] = sum * invWindow;
            }
        }
    }
}

void gaussianApproxBlurScalar(std::vector<float> &buf, int w, int h, double radiusPx)
{
    const int r = qBound(1, static_cast<int>(std::lround(radiusPx * 0.58)), 64);
    std::vector<float> tmp(buf.size(), 0.0f);
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurScalar1D(buf, tmp, w, h, r, /*horizontal=*/true);
        boxBlurScalar1D(tmp, buf, w, h, r, /*horizontal=*/false);
    }
}

} // namespace

bool hasAnyActiveConfig()
{
    if (!g_loaded.load(std::memory_order_acquire)) {
        QMutexLocker lock(&g_storeMutex);
        ensureLoadedLocked();
    }
    return g_hasAnyActive.load(std::memory_order_acquire);
}

ClipCreativeConfig configForClip(const QString &clipUid)
{
    if (clipUid.isEmpty())
        return {};
    QMutexLocker lock(&g_storeMutex);
    ensureLoadedLocked();
    return g_cache.value(clipUid);
}

void setConfigForClip(const QString &clipUid, const ClipCreativeConfig &cfg)
{
    if (clipUid.isEmpty())
        return;

    QMutexLocker lock(&g_storeMutex);
    ensureLoadedLocked();

    QSettings settings;
    settings.beginGroup(QLatin1String(kSettingsGroup));
    if (!cfg.isAnyEnabled()) {
        g_cache.remove(clipUid);
        settings.remove(clipUid);
    } else {
        g_cache.insert(clipUid, cfg);
        settings.beginGroup(clipUid);
        settings.setValue(QStringLiteral("hal_en"), cfg.halation.enabled);
        settings.setValue(QStringLiteral("hal_thr"), cfg.halation.threshold);
        settings.setValue(QStringLiteral("hal_rad"), cfg.halation.radiusPx);
        settings.setValue(QStringLiteral("hal_str"), cfg.halation.strength);

        settings.setValue(QStringLiteral("pt_en"), cfg.posterizeTime.enabled);
        settings.setValue(QStringLiteral("pt_fps"), cfg.posterizeTime.targetFps);

        settings.setValue(QStringLiteral("dof_en"), cfg.cameraDof.enabled);
        settings.setValue(QStringLiteral("dof_foc"), cfg.cameraDof.focusDistance);
        settings.setValue(QStringLiteral("dof_ap"), cfg.cameraDof.aperture);
        settings.setValue(QStringLiteral("dof_max"), cfg.cameraDof.maxBlurPx);
        settings.endGroup();
    }
    settings.endGroup();
    settings.sync();

    recomputeActiveFlagLocked();
}

void clearConfigForClip(const QString &clipUid)
{
    if (clipUid.isEmpty())
        return;
    setConfigForClip(clipUid, ClipCreativeConfig{});
}

void clearAllConfigsForTest()
{
    QMutexLocker lock(&g_storeMutex);
    g_cache.clear();
    QSettings settings;
    settings.remove(QLatin1String(kSettingsGroup));
    settings.sync();
    g_hasAnyActive.store(false, std::memory_order_release);
    g_loaded.store(true, std::memory_order_release);
}

double quantizePosterizeTime(double localSec, const PosterizeTimeParams &params)
{
    if (!params.enabled || !std::isfinite(localSec) || !(params.targetFps > 0.0))
        return localSec;
    const double fps = qBound(0.5, params.targetFps, 120.0);
    const double clamped = qMax(0.0, localSec);
    return std::floor(clamped * fps + 1e-9) / fps;
}

QImage applyFilmHalation(const QImage &src, const HalationParams &params)
{
    if (src.isNull() || !params.enabled || params.strength <= 1e-6 || params.radiusPx < 0.5)
        return src;

    QImage img = src.format() == QImage::Format_RGBA8888
        ? src.copy()
        : src.convertToFormat(QImage::Format_RGBA8888);
    const int w = img.width();
    const int h = img.height();
    if (w <= 0 || h <= 0)
        return img;

    const float thr = static_cast<float>(qBound(0.0, params.threshold, 0.99));
    const float invSpan = 1.0f / std::max(0.01f, 1.0f - thr);
    const float strength = static_cast<float>(qBound(0.0, params.strength, 2.0));

    std::vector<float> mask(static_cast<size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y) {
        const uchar *line = img.constScanLine(y);
        float *mRow = mask.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const float r = line[x * 4 + 0] * (1.0f / 255.0f);
            const float g = line[x * 4 + 1] * (1.0f / 255.0f);
            const float b = line[x * 4 + 2] * (1.0f / 255.0f);
            const float a = line[x * 4 + 3] * (1.0f / 255.0f);
            const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (luma > thr)
                mRow[x] = std::min(1.0f, (luma - thr) * invSpan) * a;
        }
    }

    gaussianApproxBlurScalar(mask, w, h, params.radiusPx);

    // Classic 35mm film anti-halation backing reflection: deep red-orange scatter
    // screen-blended onto the frame while leaving source alpha untouched.
    for (int y = 0; y < h; ++y) {
        uchar *line = img.scanLine(y);
        const float *mRow = mask.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const float bleed = mRow[x] * strength;
            if (bleed <= 1e-5f)
                continue;
            const float hR = std::min(1.0f, bleed * 1.00f);
            const float hG = std::min(1.0f, bleed * 0.28f);
            const float hB = std::min(1.0f, bleed * 0.05f);

            const float baseR = line[x * 4 + 0] * (1.0f / 255.0f);
            const float baseG = line[x * 4 + 1] * (1.0f / 255.0f);
            const float baseB = line[x * 4 + 2] * (1.0f / 255.0f);

            const float outR = 1.0f - (1.0f - baseR) * (1.0f - hR);
            const float outG = 1.0f - (1.0f - baseG) * (1.0f - hG);
            const float outB = 1.0f - (1.0f - baseB) * (1.0f - hB);

            line[x * 4 + 0] = static_cast<uchar>(qBound(0, static_cast<int>(std::lround(outR * 255.0f)), 255));
            line[x * 4 + 1] = static_cast<uchar>(qBound(0, static_cast<int>(std::lround(outG * 255.0f)), 255));
            line[x * 4 + 2] = static_cast<uchar>(qBound(0, static_cast<int>(std::lround(outB * 255.0f)), 255));
        }
    }

    return img;
}

double layerDistanceFromCamera(const Layer3DTransform &layer3D,
                               const Camera3DState &camera)
{
    // Convention: default camera at (0,0,0) looks along -Z from a nominal
    // focal plane offset of 1000 canvas px, so a layer at positionZ == 0
    // rests at distance 1000 px (in focus by default).
    const double cameraPlaneZ = static_cast<double>(camera.position.z()) + 1000.0;
    return std::abs(cameraPlaneZ - layer3D.positionZ);
}

double computeDofBlurRadiusPx(double layerDistance, const CameraDofParams &params)
{
    if (!params.enabled || !(params.aperture > 0.0) || !(params.maxBlurPx > 0.0))
        return 0.0;
    const double dist = qMax(1.0, std::abs(layerDistance));
    const double focus = qMax(1.0, params.focusDistance);
    const double delta = std::abs(dist - focus);
    if (delta < 1e-6)
        return 0.0;
    const double coc = (delta / dist) * (params.aperture / 50.0) * (params.maxBlurPx * 0.5);
    return qBound(0.0, coc, params.maxBlurPx);
}

QImage applyCameraDofBlur(const QImage &src, double radiusPx)
{
    if (src.isNull() || radiusPx < 0.5)
        return src;

    QImage img = src.format() == QImage::Format_RGBA8888
        ? src
        : src.convertToFormat(QImage::Format_RGBA8888);
    const int w = img.width();
    const int h = img.height();
    if (w <= 0 || h <= 0)
        return img;

    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> chR(n), chG(n), chB(n), chA(n);

    // Premultiply RGB by alpha so bokeh blur never bleeds opaque black halos.
    for (int y = 0; y < h; ++y) {
        const uchar *line = img.constScanLine(y);
        const size_t rowOff = static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const float a = line[x * 4 + 3] * (1.0f / 255.0f);
            chR[rowOff + x] = (line[x * 4 + 0] * (1.0f / 255.0f)) * a;
            chG[rowOff + x] = (line[x * 4 + 1] * (1.0f / 255.0f)) * a;
            chB[rowOff + x] = (line[x * 4 + 2] * (1.0f / 255.0f)) * a;
            chA[rowOff + x] = a;
        }
    }

    gaussianApproxBlurScalar(chR, w, h, radiusPx);
    gaussianApproxBlurScalar(chG, w, h, radiusPx);
    gaussianApproxBlurScalar(chB, w, h, radiusPx);
    gaussianApproxBlurScalar(chA, w, h, radiusPx);

    QImage out(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        uchar *line = out.scanLine(y);
        const size_t rowOff = static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const float a = std::clamp(chA[rowOff + x], 0.0f, 1.0f);
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (a > 1e-4f) {
                const float invA = 1.0f / a;
                r = std::clamp(chR[rowOff + x] * invA, 0.0f, 1.0f);
                g = std::clamp(chG[rowOff + x] * invA, 0.0f, 1.0f);
                b = std::clamp(chB[rowOff + x] * invA, 0.0f, 1.0f);
            }
            line[x * 4 + 0] = static_cast<uchar>(std::lround(r * 255.0f));
            line[x * 4 + 1] = static_cast<uchar>(std::lround(g * 255.0f));
            line[x * 4 + 2] = static_cast<uchar>(std::lround(b * 255.0f));
            line[x * 4 + 3] = static_cast<uchar>(std::lround(a * 255.0f));
        }
    }
    return out;
}

CreativeEffectsDialog::CreativeEffectsDialog(const QString &clipUid,
                                             const QString &clipLabel,
                                             QWidget *parent)
    : QDialog(parent)
    , m_clipUid(clipUid)
{
    setWindowTitle(QStringLiteral("フィルムハレーション / ポスタライズ時間 / 3D 被写界深度"));
    setMinimumWidth(420);

    auto *root = new QVBoxLayout(this);
    auto *header = new QLabel(QStringLiteral("対象クリップ: %1").arg(clipLabel), this);
    header->setStyleSheet(QStringLiteral("font-weight: bold;"));
    root->addWidget(header);

    // Film Halation
    auto *halGroup = new QGroupBox(QStringLiteral("フィルムハレーション (Film Halation)"), this);
    auto *halForm = new QFormLayout(halGroup);
    m_halationCheck = new QCheckBox(QStringLiteral("有効にする"), halGroup);
    m_halationThreshold = new QDoubleSpinBox(halGroup);
    m_halationThreshold->setRange(0.05, 0.99);
    m_halationThreshold->setSingleStep(0.05);
    m_halationThreshold->setDecimals(2);
    m_halationRadius = new QDoubleSpinBox(halGroup);
    m_halationRadius->setRange(1.0, 64.0);
    m_halationRadius->setSuffix(QStringLiteral(" px"));
    m_halationStrength = new QDoubleSpinBox(halGroup);
    m_halationStrength->setRange(0.0, 2.0);
    m_halationStrength->setSingleStep(0.1);
    m_halationStrength->setDecimals(2);
    halForm->addRow(m_halationCheck);
    halForm->addRow(QStringLiteral("ハイライト閾値:"), m_halationThreshold);
    halForm->addRow(QStringLiteral("滲み半径:"), m_halationRadius);
    halForm->addRow(QStringLiteral("強度:"), m_halationStrength);
    root->addWidget(halGroup);

    // Posterize Time
    auto *ptGroup = new QGroupBox(QStringLiteral("ポスタライズ時間 (Posterize Time)"), this);
    auto *ptForm = new QFormLayout(ptGroup);
    m_posterizeCheck = new QCheckBox(QStringLiteral("フレームレートをステップ化"), ptGroup);
    m_posterizeFps = new QDoubleSpinBox(ptGroup);
    m_posterizeFps->setRange(0.5, 120.0);
    m_posterizeFps->setDecimals(1);
    m_posterizeFps->setSuffix(QStringLiteral(" fps"));
    ptForm->addRow(m_posterizeCheck);
    ptForm->addRow(QStringLiteral("ターゲット FPS:"), m_posterizeFps);
    root->addWidget(ptGroup);

    // 3D Camera DOF
    auto *dofGroup = new QGroupBox(QStringLiteral("3D カメラ被写界深度 (DOF / Bokeh)"), this);
    auto *dofForm = new QFormLayout(dofGroup);
    m_dofCheck = new QCheckBox(QStringLiteral("3D レイヤーの Z 距離に応じてボケを適用"), dofGroup);
    m_dofFocus = new QDoubleSpinBox(dofGroup);
    m_dofFocus->setRange(10.0, 10000.0);
    m_dofFocus->setSingleStep(50.0);
    m_dofFocus->setSuffix(QStringLiteral(" px"));
    m_dofAperture = new QDoubleSpinBox(dofGroup);
    m_dofAperture->setRange(1.0, 300.0);
    m_dofAperture->setSingleStep(5.0);
    m_dofMaxBlur = new QDoubleSpinBox(dofGroup);
    m_dofMaxBlur->setRange(0.0, 64.0);
    m_dofMaxBlur->setSuffix(QStringLiteral(" px"));
    dofForm->addRow(m_dofCheck);
    dofForm->addRow(QStringLiteral("フォーカス距離:"), m_dofFocus);
    dofForm->addRow(QStringLiteral("絞り (Aperture):"), m_dofAperture);
    dofForm->addRow(QStringLiteral("最大ボケ半径:"), m_dofMaxBlur);
    root->addWidget(dofGroup);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &CreativeEffectsDialog::saveAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (QPushButton *resetBtn = buttons->button(QDialogButtonBox::Reset)) {
        connect(resetBtn, &QPushButton::clicked, this, [this]() {
            applyFromConfig(ClipCreativeConfig{});
        });
    }
    root->addWidget(buttons);

    applyFromConfig(configForClip(m_clipUid));
}

ClipCreativeConfig CreativeEffectsDialog::currentConfig() const
{
    ClipCreativeConfig cfg;
    cfg.halation.enabled = m_halationCheck->isChecked();
    cfg.halation.threshold = m_halationThreshold->value();
    cfg.halation.radiusPx = m_halationRadius->value();
    cfg.halation.strength = m_halationStrength->value();

    cfg.posterizeTime.enabled = m_posterizeCheck->isChecked();
    cfg.posterizeTime.targetFps = m_posterizeFps->value();

    cfg.cameraDof.enabled = m_dofCheck->isChecked();
    cfg.cameraDof.focusDistance = m_dofFocus->value();
    cfg.cameraDof.aperture = m_dofAperture->value();
    cfg.cameraDof.maxBlurPx = m_dofMaxBlur->value();
    return cfg;
}

void CreativeEffectsDialog::applyFromConfig(const ClipCreativeConfig &cfg)
{
    m_halationCheck->setChecked(cfg.halation.enabled);
    m_halationThreshold->setValue(cfg.halation.threshold);
    m_halationRadius->setValue(cfg.halation.radiusPx);
    m_halationStrength->setValue(cfg.halation.strength);

    m_posterizeCheck->setChecked(cfg.posterizeTime.enabled);
    m_posterizeFps->setValue(cfg.posterizeTime.targetFps);

    m_dofCheck->setChecked(cfg.cameraDof.enabled);
    m_dofFocus->setValue(cfg.cameraDof.focusDistance);
    m_dofAperture->setValue(cfg.cameraDof.aperture);
    m_dofMaxBlur->setValue(cfg.cameraDof.maxBlurPx);
}

void CreativeEffectsDialog::saveAndAccept()
{
    setConfigForClip(m_clipUid, currentConfig());
    accept();
}

} // namespace creative
