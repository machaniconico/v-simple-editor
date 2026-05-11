#include "GLPreview.h"
#include "Overlay.h"
#include "Timeline.h"
#include "VideoPlayer.h"
#include "AdjustmentLayer.h"
#include "Camera3D.h"
#include "SurfaceTool.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <QElapsedTimer>
#include <QtGlobal>
#include <QDateTime>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QMatrix3x3>
#include <QOpenGLContext>
#include <QDebug>
#include <QPainter>
#include <QPen>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QFontMetrics>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// windef.h leaks `near`/`far` and min/max macros that collide with C++
// identifiers used elsewhere in this file (see hitTestTextToolHandle).
#undef near
#undef far
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// WGL_NV_DX_interop2 typedefs. Kept in the .cpp so windows.h doesn't leak
// through GLPreview.h to the rest of the codebase. WGL_ACCESS_*_NV constants
// are defined here for Section B/C use.
#ifndef WGL_ACCESS_READ_ONLY_NV
#define WGL_ACCESS_READ_ONLY_NV  0x00000000
#define WGL_ACCESS_READ_WRITE_NV 0x00000001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x00000002
#endif

// PFNWGLDXSETRESOURCESHAREHANDLENVPROC slot is resolved in Section C when
// shared-texture creation needs it; Section A does not call it.
typedef BOOL    (WINAPI *PFNWGLDXSETRESOURCESHAREHANDLENVPROC)(void *dxObject, HANDLE shareHandle);
typedef HANDLE  (WINAPI *PFNWGLDXOPENDEVICENVPROC)(void *dxDevice);
typedef BOOL    (WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE  (WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void *dxObject, GLuint name, GLenum type, GLenum access);
typedef BOOL    (WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL    (WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL    (WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);

typedef const char * (WINAPI *PFNWGLGETEXTENSIONSSTRINGARBPROC)(HDC hdc);

namespace {
PFNWGLDXOPENDEVICENVPROC       gWglDXOpenDeviceNV       = nullptr;
PFNWGLDXCLOSEDEVICENVPROC      gWglDXCloseDeviceNV      = nullptr;
PFNWGLDXREGISTEROBJECTNVPROC   gWglDXRegisterObjectNV   = nullptr;
PFNWGLDXUNREGISTEROBJECTNVPROC gWglDXUnregisterObjectNV = nullptr;
PFNWGLDXLOCKOBJECTSNVPROC      gWglDXLockObjectsNV      = nullptr;
PFNWGLDXUNLOCKOBJECTSNVPROC    gWglDXUnlockObjectsNV    = nullptr;
} // namespace
#endif // Q_OS_WIN

namespace {
// Read VEDITOR_GL_INTEROP once and cache; mirrors veditorTickTraceEnabled()
// pattern in VideoPlayer.cpp so production with the envvar unset pays a
// single env read at first call and zero work after.
bool veditorGlInteropEnabled()
{
    static const bool kEnabled = qEnvironmentVariableIntValue("VEDITOR_GL_INTEROP") != 0;
    return kEnabled;
}

// Phase 1e Win #11 — VEDITOR_STALL_TRACE=1 reuses the same envvar as
// VideoPlayer/AudioMixer so a single switch covers all instrumented
// stall sites. paintGL is on the Qt GL render thread; renderPendingD3D11Frame's
// wglDXLockObjectsNV is the only documented multi-frame GPU-CPU sync barrier
// in this translation unit and was not covered by Win #10's decode-thread
// probes. Default off.
bool stallTraceEnabled()
{
    static const bool kEnabled = qEnvironmentVariableIntValue("VEDITOR_STALL_TRACE") != 0;
    return kEnabled;
}
constexpr qint64 kStallThresholdInteropLockMs = 50;

struct PreviewClipMotion {
    bool valid = false;
    int trackIdx = -1;
    int clipIdx = -1;
    double scale = 1.0;
    double dx = 0.0;
    double dy = 0.0;
    double rotation2D = 0.0;
    double opacity = 1.0;
    bool is3DLayer = false;
    Layer3DTransform layer3D;
};

bool fuzzyMatchMotionValue(double a, double b, double eps = 1e-4)
{
    return std::abs(a - b) <= eps;
}

QImage applyPlanarRotation(const QImage &image, double degrees)
{
    if (image.isNull() || std::abs(degrees) <= 1e-4)
        return image;

    const QImage src = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage rotated(src.size(), QImage::Format_ARGB32_Premultiplied);
    rotated.fill(Qt::transparent);

    QPainter painter(&rotated);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(src.width() * 0.5, src.height() * 0.5);
    painter.rotate(degrees);
    painter.translate(-src.width() * 0.5, -src.height() * 0.5);
    painter.drawImage(0, 0, src);
    painter.end();
    return rotated;
}

PreviewClipMotion makePreviewMotion(const ClipInfo &clip, int trackIdx, int clipIdx)
{
    PreviewClipMotion motion;
    motion.valid = true;
    motion.trackIdx = trackIdx;
    motion.clipIdx = clipIdx;
    motion.scale = clip.videoScale;
    motion.dx = clip.videoDx;
    motion.dy = clip.videoDy;
    motion.rotation2D = clip.rotation2DDegrees;
    motion.opacity = clip.opacity;
    motion.is3DLayer = clip.is3DLayer;
    motion.layer3D = clip.is3DLayer ? clip.layer3D : Layer3DTransform{};
    return motion;
}

PreviewClipMotion resolvePreviewClipMotion(const Timeline *timeline, double timelineSec,
                                           bool compositeBaked,
                                           double currentScale, double currentDx, double currentDy)
{
    if (!timeline)
        return {};

    struct ActiveClipRef {
        int trackIdx = -1;
        int clipIdx = -1;
        const ClipInfo *clip = nullptr;
    };

    QVector<ActiveClipRef> activeClips;
    const auto &videoTracks = timeline->videoTracks();
    for (int trackIdx = 0; trackIdx < videoTracks.size(); ++trackIdx) {
        const auto *track = videoTracks[trackIdx];
        if (!track || track->isHidden())
            continue;

        double accumSec = 0.0;
        const auto &clips = track->clips();
        for (int clipIdx = 0; clipIdx < clips.size(); ++clipIdx) {
            const ClipInfo &clip = clips[clipIdx];
            accumSec += qMax(0.0, clip.leadInSec);
            const double durationSec = clip.effectiveDuration();
            const double startSec = accumSec;
            const double endSec = startSec + durationSec;
            if (durationSec > 0.0
                && timelineSec >= startSec - 1e-6
                && timelineSec <= endSec + 1e-6) {
                activeClips.append(ActiveClipRef{trackIdx, clipIdx, &clip});
                break;
            }
            accumSec = endSec;
        }
    }

    if (activeClips.isEmpty())
        return {};
    if (compositeBaked && activeClips.size() > 1)
        return {};
    if (activeClips.size() == 1)
        return makePreviewMotion(*activeClips.first().clip,
                                 activeClips.first().trackIdx,
                                 activeClips.first().clipIdx);

    for (int trackIdx = 0; trackIdx < videoTracks.size(); ++trackIdx) {
        const auto *track = videoTracks[trackIdx];
        if (!track)
            continue;
        const int selectedClipIdx = track->selectedClip();
        if (selectedClipIdx < 0)
            continue;
        for (const ActiveClipRef &ref : activeClips) {
            if (ref.trackIdx == trackIdx && ref.clipIdx == selectedClipIdx)
                return makePreviewMotion(*ref.clip, ref.trackIdx, ref.clipIdx);
        }
    }

    for (const ActiveClipRef &ref : activeClips) {
        if (fuzzyMatchMotionValue(ref.clip->videoScale, currentScale)
            && fuzzyMatchMotionValue(ref.clip->videoDx, currentDx)
            && fuzzyMatchMotionValue(ref.clip->videoDy, currentDy)) {
            return makePreviewMotion(*ref.clip, ref.trackIdx, ref.clipIdx);
        }
    }

    return makePreviewMotion(*activeClips.first().clip,
                             activeClips.first().trackIdx,
                             activeClips.first().clipIdx);
}
} // namespace

// Vertex shader — simple fullscreen quad
static const char *vertexShaderSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

// Fragment shader — color correction + color grading pipeline on GPU
static const char *fragmentShaderSrc = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform bool uEffectsEnabled;
uniform float uClipOpacity;

// Color correction uniforms
uniform float uBrightness;   // -100 to 100
uniform float uContrast;     // -100 to 100
uniform float uSaturation;   // -100 to 100
uniform float uHue;          // -180 to 180
uniform float uTemperature;  // -100 to 100
uniform float uTint;         // -100 to 100
uniform float uGamma;        // 0.1 to 3.0
uniform float uHighlights;   // -100 to 100
uniform float uShadows;      // -100 to 100
uniform float uExposure;     // -3.0 to 3.0

// US-WIRE-2: Lift/Gamma/Gain color wheels (DaVinci Resolve style)
// xyz = RGB channel scalar, w = Luma scalar applied across all channels
uniform vec4 uLift;      // additive offset
uniform vec4 uLggGamma;  // power-curve exponent denominator (renamed from uGamma to avoid collision with scalar uGamma at line ~117)
uniform vec4 uGain;      // multiplicative scaling

// 3D LUT uniforms
uniform sampler3D uLut3D;
uniform float uLutIntensity;  // 0.0 to 1.0
uniform bool uLutEnabled;

// US-CG-1: RGB Curves LUT (256x4 RGBA8). Rows: 0=R, 1=G, 2=B, 3=Luma.
uniform sampler2D uCurveLut;
uniform bool uCurvesEnabled;

// US-CG-2: White-balance gain triple, applied at the very top of the grade
// chain (BEFORE LGG, curves, and the .cube LUT). Identity = vec3(1.0).
uniform vec3 uWb;

// US-CG-3: Radial vignette / Power Window. Applied AFTER curves and BEFORE
// the .cube LUT. Identity = uVigAmount==0 (factor=1.0 → no-op).
uniform float uVigAmount;     // -1..+1 (negative=darken, positive=lighten)
uniform float uVigMid;        //  0..1  (radius % of frame, 0.7 default)
uniform float uVigRound;      // -1..+1 (0=circular, ±1=squareness)
uniform float uVigFeather;    //  0..1  (edge softness, 0.3 default)

// US-EF-1: Chroma Key (Premiere Ultra Key / Resolve 3D Keyer simplified).
// Applied at the VERY TOP of the compose path — BEFORE WB/LGG/curves/
// vignette/LUT — so HSL gating + spill suppression operate on raw frame
// colour. uChromaEnabled=false is a free no-op.
uniform bool  uChromaEnabled;
uniform vec3  uChromaKeyHsl;  // (H, S, L) of key colour, each in [0..1]
uniform vec3  uChromaTol;     // (hueTol, satTol, lumTol), each in [0..1]
uniform float uChromaSpill;   // 0..1 spill desaturation strength
uniform float uChromaSoft;    // 0..1 smoothstep edge half-width

// US-EF-3: HSL Qualifier (DaVinci secondary grading). Sits AFTER chroma key
// and BEFORE WB so the qualifier acts on raw frame colour. Builds a 3D HSL
// gating mask (circular hue distance × sat range × luma range) and applies
// a SECONDARY lift/gamma/gain only inside the qualified region via mix().
// uHslqEnabled=false is a free no-op (entire stage skipped).
uniform bool  uHslqEnabled;
uniform float uHslqHueCenter;   // degrees [0..360]
uniform float uHslqHueRange;    // degrees [0..180]
uniform vec2  uHslqSatRange;    // (min, max) in [0..1]
uniform vec2  uHslqLumaRange;   // (min, max) in [0..1]
uniform float uHslqSoftness;    // [0..50] edge slop (degrees / pct)
uniform vec3  uHslqLift;        // per-channel additive offset
uniform vec3  uHslqGamma;       // per-channel gamma (identity 1.0)
uniform vec3  uHslqGain;        // per-channel multiplier (identity 1.0)

// US-CG-4: Hue vs Saturation curve (DaVinci Resolve color page parity).
// Sampled AFTER the .cube LUT and BEFORE the mask wrap so the curve sees
// the final graded colour. uHueVsSatEnabled=false is a free no-op.
//   uHueVsSatLut : 256x1 R-only texture; sample.r is the sat multiplier
//                  for that hue bin (0.0 = full desaturate, 1.0 = identity,
//                  2.0 = double saturation).
uniform bool      uHueVsSatEnabled;
uniform sampler2D uHueVsSatLut;

// US-EF-2: Mask Animation (DaVinci Power Window simplified). Wraps the
// entire grade chain — `ungraded` is captured before chroma key, the grade
// chain mutates `color`, and at the end we mix(ungraded, graded, weight).
// uMaskEnabled=false branches around the mix entirely → free no-op (output
// is bit-identical to the previous shader behaviour).
uniform bool  uMaskEnabled;
uniform bool  uMaskEllipse;   // false=Rect, true=Ellipse
uniform bool  uMaskInvert;
uniform float uMaskFeather;   // 0..1 edge softness
uniform vec4  uMaskRect;      // (x, y, w, h) normalized in vTexCoord space

// 0=sRGB, 1=PQ, 2=HLG. Non-zero applies inverse EOTF + Hable tone map before grading.
uniform int uHdrTransfer;

// US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens Distortion.
// uLensDistortion is applied as a texture-coordinate transform at the very
// TOP of main() (BEFORE the texture sample), while uBlurRadius and
// uSharpenAmount drive POST stages at the END of main() — after the entire
// grade chain (chroma key → HSL Q → WB → exposure → ... → LGG → curves →
// vignette → LUT → mask wrap). All three default to identity (0) so the
// stage is a free no-op until the user touches the エフェクト sliders.
uniform float uSharpenAmount;   // 0..200; shader multiplies by 0.01
uniform float uBlurRadius;      // 0..50 px; identity at 0
uniform float uLensDistortion;  // -100..+100; shader multiplies by 0.01

// US-3D: 3-axis rotation + perspective foreshortening (Premiere "Basic 3D" /
// Resolve "Transform" 3D rotation parity). Composed AFTER lens distortion
// (its texture-coord transform) and BEFORE the texture sample, via a
// fragment-shader inverse warp: shift to centre, multiply by uRot3D,
// perspective-divide by (1 + p.z * uPerspectiveDist), shift back. Out-of-
// bounds sample coordinates render black so the rotated quad letterboxes
// against the canvas. uRot3DEnabled=false is a free no-op.
uniform bool  uRot3DEnabled;
uniform mat3  uRot3D;
uniform float uPerspectiveDist; // inverse FOV; 0.1..10.0 (default 2.0)

// US-INT-4: stabilizer pre-warp. uStab is a 2D inverse-affine matrix in
// homogeneous form (last row [0 0 1]) that operates on (lensCoord-0.5),
// then re-shifts. Composed BEFORE the 3D-rotate warp so user 3D-rotate is
// preserved (operations stack). Identity = uStabEnabled=false = free no-op.
uniform bool  uStabEnabled;
uniform mat3  uStab;

// GPU Video Effects — run after CC.
uniform bool  uFxBlurEnable;
uniform float uFxBlurRadius;   // pixels (shader clamps to a small box kernel)
uniform vec2  uFxTexSize;      // texture pixel size for blur sampling
uniform bool  uFxNoiseEnable;
uniform float uFxNoiseAmount;  // 0..1 luma jitter
uniform bool  uFxSepiaEnable;
uniform float uFxSepiaStrength;     // 0..1 lerp
uniform bool  uFxGrayEnable;
uniform float uFxGrayStrength;      // 0..1 lerp
uniform bool  uFxInvertEnable;
uniform float uFxInvertStrength;    // 0..1 lerp
uniform bool  uFxVignetteEnable;
uniform float uFxVignetteIntensity; // 0..1 strength
uniform float uFxVignetteRadius;    // 0..1 inner radius
uniform float uFxTime;         // seconds, seeds the noise hash
uniform bool  uFxSharpenEnable;
uniform float uFxSharpenAmount;  // 0..2.0
uniform bool  uFxMosaicEnable;
uniform float uFxMosaicSize;
uniform bool  uFxChromaKeyEnable;
uniform vec3  uFxChromaKey;
uniform float uFxChromaTolerance;

vec3 pqInverseEotf(vec3 E) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 Ep = pow(max(E, vec3(0.0)), vec3(1.0 / m2));
    vec3 num = max(Ep - vec3(c1), vec3(0.0));
    vec3 den = vec3(c2) - vec3(c3) * Ep;
    return pow(num / den, vec3(1.0 / m1));
}

vec3 hlgInverseOetf(vec3 E) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 result;
    result.r = (E.r <= 0.5) ? (E.r * E.r / 3.0) : (exp((E.r - c) / a) + b) / 12.0;
    result.g = (E.g <= 0.5) ? (E.g * E.g / 3.0) : (exp((E.g - c) / a) + b) / 12.0;
    result.b = (E.b <= 0.5) ? (E.b * E.b / 3.0) : (exp((E.b - c) / a) + b) / 12.0;
    return result;
}

vec3 hableCurve(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 hableToneMap(vec3 color) {
    const float W = 11.2;
    vec3 mapped = hableCurve(color * 2.0);
    vec3 whiteScale = vec3(1.0) / hableCurve(vec3(W));
    return mapped * whiteScale;
}

vec3 adjustExposure(vec3 color, float exposure) {
    return color * pow(2.0, exposure);
}

vec3 adjustBrightnessContrast(vec3 color, float brightness, float contrast) {
    vec3 c = color + brightness / 100.0;
    float factor = (100.0 + contrast) / 100.0;
    factor *= factor;
    c = (c - 0.5) * factor + 0.5;
    return c;
}

vec3 adjustHighlightsShadows(vec3 color, float highlights, float shadows) {
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float hWeight = lum * lum;
    float sWeight = (1.0 - lum) * (1.0 - lum);
    float adjust = highlights / 100.0 * hWeight + shadows / 100.0 * sWeight;
    return color + adjust;
}

vec3 adjustSaturation(vec3 color, float saturation) {
    float factor = (saturation + 100.0) / 100.0;
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(lum), color, factor);
}

vec3 adjustHue(vec3 color, float hue) {
    float angle = radians(hue);
    float cosA = cos(angle);
    float sinA = sin(angle);

    mat3 hueRotation = mat3(
        0.213 + 0.787 * cosA - 0.213 * sinA,
        0.213 - 0.213 * cosA + 0.143 * sinA,
        0.213 - 0.213 * cosA - 0.787 * sinA,
        0.715 - 0.715 * cosA - 0.715 * sinA,
        0.715 + 0.285 * cosA + 0.140 * sinA,
        0.715 - 0.715 * cosA + 0.715 * sinA,
        0.072 - 0.072 * cosA + 0.928 * sinA,
        0.072 - 0.072 * cosA - 0.283 * sinA,
        0.072 + 0.928 * cosA + 0.072 * sinA
    );

    return hueRotation * color;
}

vec3 adjustTemperatureTint(vec3 color, float temperature, float tint) {
    float rShift = temperature * 0.005;
    float bShift = -temperature * 0.005;
    float gShift = -tint * 0.003;
    float mShift = tint * 0.002;
    color.r += rShift + mShift;
    color.g += gShift;
    color.b += bShift + mShift;
    return color;
}

vec3 adjustGamma(vec3 color, float gamma) {
    float invGamma = 1.0 / gamma;
    return pow(max(color, vec3(0.0)), vec3(invGamma));
}

// US-EF-1: HSL helpers used by the Chroma Key stage. RGB↔HSL conversions
// match the formulas used in QColor::getHslF so the GPU mask matches the
// CPU swatch the user picked in the colour dialog.
vec3 rgbToHsl(vec3 c) {
    float maxC = max(max(c.r, c.g), c.b);
    float minC = min(min(c.r, c.g), c.b);
    float L = (maxC + minC) * 0.5;
    float H = 0.0, S = 0.0;
    if (maxC > minC) {
        float d = maxC - minC;
        S = (L > 0.5) ? d / (2.0 - maxC - minC) : d / (maxC + minC);
        if (maxC == c.r) H = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
        else if (maxC == c.g) H = (c.b - c.r) / d + 2.0;
        else H = (c.r - c.g) / d + 4.0;
        H /= 6.0;
    }
    return vec3(H, S, L);
}
float hue2rgb(float p, float q, float t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0/2.0) return q;
    if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
    return p;
}
vec3 hslToRgb(vec3 hsl) {
    float h = hsl.x, s = hsl.y, l = hsl.z;
    if (s == 0.0) return vec3(l);
    float q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    float p = 2.0 * l - q;
    return vec3(hue2rgb(p, q, h + 1.0/3.0), hue2rgb(p, q, h), hue2rgb(p, q, h - 1.0/3.0));
}

// US-WIRE-2: Lift/Gamma/Gain color wheels
// Order: Lift → Gamma → Gain applied BEFORE LUT (grading → LUT, DaVinci-style)
// Math per pixel RGB in [0,1] linear-ish space:
//   1. Lift:  c1 = c + lift_rgb + lift_luma  (additive, can be negative)
//   2. Gamma: c2 = pow(max(c1,0.0), 1.0 / max(gamma_rgb * gamma_luma, 1e-3))
//   3. Gain:  c3 = c2 * gain_rgb * gain_luma
vec3 applyLiftGammaGain(vec3 color) {
    // Stage 1 — Lift: additive offset, applied uniformly (not shadow-weighted)
    vec3 c1 = color + uLift.rgb + uLift.www;

    // Stage 2 — Gamma: power curve; denominator clamped >= 1e-3 for safety
    vec3 gammaTotal = max(uLggGamma.rgb * uLggGamma.w, vec3(1e-3));
    vec3 c2 = pow(max(c1, vec3(0.0)), vec3(1.0) / gammaTotal);

    // Stage 3 — Gain: multiplicative scaling, hard-capped at 16
    vec3 gainTotal = min(uGain.rgb * uGain.w, vec3(16.0));
    vec3 c3 = c2 * gainTotal;

    c3 = clamp(c3, 0.0, 1.0);
    return c3;
}

vec3 fxBlur(vec2 uv) {
    float r = clamp(uFxBlurRadius, 0.0, 8.0);
    if (r < 0.5 || uFxTexSize.x <= 0.0) return texture(uTexture, uv).rgb;
    vec2 px = 1.0 / uFxTexSize;
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    int ri = int(ceil(r));
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            if (abs(dx) > ri || abs(dy) > ri) continue;
            vec2 off = vec2(float(dx), float(dy)) * px;
            acc += texture(uTexture, uv + off).rgb;
            wsum += 1.0;
        }
    }
    return (wsum > 0.0) ? acc / wsum : texture(uTexture, uv).rgb;
}
)"
R"(
float fxHash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 fxApplyNoise(vec3 color, vec2 uv) {
    float n = fxHash(uv * uFxTexSize + vec2(uFxTime, uFxTime * 1.3));
    float j = (n - 0.5) * uFxNoiseAmount;
    return clamp(color + vec3(j), 0.0, 1.0);
}

vec3 fxApplySepia(vec3 color, float t) {
    vec3 sepia = vec3(
        dot(color, vec3(0.393, 0.769, 0.189)),
        dot(color, vec3(0.349, 0.686, 0.168)),
        dot(color, vec3(0.272, 0.534, 0.131))
    );
    return mix(color, sepia, t);
}

vec3 fxApplyGray(vec3 color, float t) {
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return mix(color, vec3(lum), t);
}

vec3 fxApplyInvert(vec3 color, float t) {
    return mix(color, vec3(1.0) - color, t);
}

vec3 fxApplyVignette(vec3 color, vec2 uv) {
    float dist = distance(uv, vec2(0.5));
    float edge = uFxVignetteRadius;
    float falloff = smoothstep(edge, edge + 0.35, dist);
    float factor = 1.0 - falloff * uFxVignetteIntensity;
    return color * factor;
}

vec3 applySharpen(sampler2D tex, vec2 uv, vec2 texelSize, float amount) {
    vec3 blurred = vec3(0.0);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            blurred += texture(tex, uv + vec2(float(dx), float(dy)) * texelSize).rgb;
    blurred /= 9.0;
    vec3 color = texture(tex, uv).rgb;
    return color + amount * (color - blurred);
}

vec2 applyMosaic(vec2 uv, float mosaicSize) {
    float blockCount = max(2.0, 100.0 - mosaicSize);
    return floor(uv * blockCount) / blockCount;
}

vec4 applyChromaKey(vec3 color, vec3 key, float tolerance) {
    return vec4(color, smoothstep(tolerance, tolerance + 0.05, distance(color, key)));
}

void main() {
    // US-EF-4: Lens Distortion — applied at the very TOP of main() as a
    // texture-coordinate transform BEFORE the texture sample. amount<0 →
    // barrel (frame edges bow outward), amount>0 → pincushion. Identity at
    // 0 means lensCoord == vTexCoord so the texture lookup is unchanged.
    vec2 lensCoord = vTexCoord;
    if (abs(uLensDistortion) > 0.001) {
        vec2 c = vTexCoord - 0.5;
        float r2 = dot(c, c);
        float k = 1.0 + uLensDistortion * 0.01 * r2;
        lensCoord = c * k + 0.5;
    }
    // US-INT-4: stabilizer inverse-affine pre-warp. Applied BEFORE the
    // 3D-rotate stage so the user's 3D rotation matrix is preserved (the
    // two transforms stack — stabilization undoes camera shake on the
    // source frame; 3D rotate is then applied on top of the stabilized
    // image). Identity = free no-op via uStabEnabled=false.
    if (uStabEnabled) {
        vec3 q = uStab * vec3(lensCoord.x - 0.5, lensCoord.y - 0.5, 1.0);
        lensCoord = vec2(q.x + 0.5, q.y + 0.5);
    }

    // US-3D: 3-axis rotation + perspective foreshortening. Composed AFTER
    // the lens distortion warp so both transforms stack into the final
    // sample coord. Inverse mapping: shift to centre, multiply by the CPU-
    // built 3x3 rotation matrix, perspective-divide along z, shift back.
    // Out-of-bounds samples short-circuit to black so the rotated quad
    // letterboxes against the canvas instead of repeating texture edges.
    if (uRot3DEnabled) {
        vec2 c = lensCoord - 0.5;
        vec3 p = uRot3D * vec3(c.x, c.y, 0.0);
        float w = 1.0 + p.z * uPerspectiveDist;
        if (w < 1e-3) w = 1e-3;
        vec2 rotCoord = vec2(p.x, p.y) / w + 0.5;
        if (rotCoord.x < 0.0 || rotCoord.x > 1.0 ||
            rotCoord.y < 0.0 || rotCoord.y > 1.0) {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        lensCoord = rotCoord;
    }
    vec2 sampleUv = uFxMosaicEnable ? applyMosaic(lensCoord, uFxMosaicSize) : lensCoord;
    vec4 texColor = texture(uTexture, sampleUv);
    vec3 color = uFxBlurEnable ? fxBlur(lensCoord) : texColor.rgb;

    if (uHdrTransfer == 1) {
        color = pqInverseEotf(color);
        color = hableToneMap(color * 10.0);
    } else if (uHdrTransfer == 2) {
        color = hlgInverseOetf(color);
        color = hableToneMap(color);
    }

    if (uEffectsEnabled) {
        // US-EF-2: Mask Animation. Snapshot the raw colour BEFORE any grade
        // stage runs so the mask wrap can mix(ungraded, graded, weight) at
        // the end of the chain. Cheap (one vec3 copy); when uMaskEnabled is
        // false the wrap branch is skipped entirely.
        vec3 ungraded = color;

        // US-EF-1: Chroma Key — gates raw frame colour BEFORE every grade
        // stage so the picked HSL distance reflects what the user clicked
        // in the swatch. Distance is computed in HSL space with the hue
        // axis treated as circular; the smoothstep mask softens edges and
        // a hue-proximity-weighted desaturation removes the green/blue
        // spill that bleeds onto the foreground subject.
        if (uChromaEnabled) {
            vec3 hsl = rgbToHsl(color.rgb);
            vec3 d = abs(hsl - uChromaKeyHsl);
            d.x = min(d.x, 1.0 - d.x);  // hue is circular
            vec3 normalizedTol = max(uChromaTol, vec3(0.0001));
            float dist = length(d / normalizedTol);
            float alpha = smoothstep(1.0 - uChromaSoft, 1.0 + uChromaSoft, dist);
            float keyProx = 1.0 - smoothstep(0.0, 1.0, d.x / normalizedTol.x);
            hsl.y *= mix(1.0, 1.0 - uChromaSpill, keyProx);
            color.rgb = hslToRgb(hsl);
            color.rgb *= alpha;
            texColor.a *= alpha;
        }

        // US-EF-3: HSL Qualifier (DaVinci secondary grading). Build a 3D
        // gating mask in HSL space (circular hue distance × sat range ×
        // luma range), then apply a secondary lift/gamma/gain ONLY inside
        // the qualified region via mix(). Sits between chroma key (which
        // gates raw colour) and WB (which globally tints) so the qualifier
        // sees the raw-but-keyed frame. uHslqEnabled=false skips the stage.
        if (uHslqEnabled) {
            vec3 hsl = rgbToHsl(color.rgb);
            // Circular hue distance, in degrees [0..180].
            float hDist = abs(hsl.x * 360.0 - uHslqHueCenter);
            hDist = min(hDist, 360.0 - hDist);
            float hWeight = 1.0 - smoothstep(uHslqHueRange - uHslqSoftness,
                                             uHslqHueRange + uHslqSoftness,
                                             hDist);
            float satEdge = uHslqSoftness * 0.01;
            float sWeight = smoothstep(uHslqSatRange.x - satEdge,
                                       uHslqSatRange.x, hsl.y) *
                           (1.0 - smoothstep(uHslqSatRange.y,
                                             uHslqSatRange.y + satEdge,
                                             hsl.y));
            float lWeight = smoothstep(uHslqLumaRange.x - satEdge,
                                       uHslqLumaRange.x, hsl.z) *
                           (1.0 - smoothstep(uHslqLumaRange.y,
                                             uHslqLumaRange.y + satEdge,
                                             hsl.z));
            float qMask = clamp(hWeight * sWeight * lWeight, 0.0, 1.0);

            vec3 graded = color.rgb + uHslqLift;
            graded = pow(max(graded, vec3(0.0)),
                         vec3(1.0) / max(uHslqGamma, vec3(1e-3)));
            graded *= uHslqGain;
            color.rgb = mix(color.rgb, graded, qMask);
        }

        // US-CG-2: White-balance multiply at the VERY TOP of the grade chain
        // (BEFORE LGG, curves, .cube LUT, and the legacy CPU-style stages).
        // Identity uWb=vec3(1.0) is a free no-op.
        if (uWb != vec3(1.0))
            color *= uWb;

        // Apply color correction pipeline (same order as CPU)
        if (uExposure != 0.0)
            color = adjustExposure(color, uExposure);
        if (uBrightness != 0.0 || uContrast != 0.0)
            color = adjustBrightnessContrast(color, uBrightness, uContrast);
        if (uHighlights != 0.0 || uShadows != 0.0)
            color = adjustHighlightsShadows(color, uHighlights, uShadows);
        if (uSaturation != 0.0)
            color = adjustSaturation(color, uSaturation);
        if (uHue != 0.0)
            color = adjustHue(color, uHue);
        if (uTemperature != 0.0 || uTint != 0.0)
            color = adjustTemperatureTint(color, uTemperature, uTint);
        if (uGamma != 1.0)
            color = adjustGamma(color, uGamma);

        // US-WIRE-2: Lift/Gamma/Gain color wheels (DaVinci Resolve style)
        // Identity: vec4(0,0,0,0) / vec4(1,1,1,1) / vec4(1,1,1,1) = no-op
        if (uLift != vec4(0.0) || uLggGamma != vec4(1.0, 1.0, 1.0, 1.0) || uGain != vec4(1.0, 1.0, 1.0, 1.0))
            color = applyLiftGammaGain(color);

        // US-CG-1: RGB Curves stage. The 256x4 LUT has rows
        //   0=R, 1=G, 2=B, 3=Luma (sample at v = 0.125, 0.375, 0.625, 0.875).
        // Per-channel curves remap c.r/c.g/c.b independently; the Luma curve
        // remaps perceptual luma and redistributes the offset across RGB so
        // hue stays roughly constant.
        if (uCurvesEnabled) {
            vec3 cclamped = clamp(color, 0.0, 1.0);
            color.r = texture(uCurveLut, vec2(cclamped.r, 0.125)).r;
            color.g = texture(uCurveLut, vec2(cclamped.g, 0.375)).g;
            color.b = texture(uCurveLut, vec2(cclamped.b, 0.625)).b;
            float Y = dot(clamp(color, 0.0, 1.0), vec3(0.299, 0.587, 0.114));
            float Ycurved = texture(uCurveLut, vec2(Y, 0.875)).a;
            float dY = Ycurved - Y;
            color += vec3(dY);
        }

        // US-CG-3: Radial vignette / Power Window. Sits AFTER curves and
        // BEFORE the .cube LUT so the LUT can still recolor the falloff.
        // amount=0 is a free no-op (factor stays at 1.0).
        if (abs(uVigAmount) > 0.0001) {
            vec2 uv = vTexCoord - 0.5;
            float r = length(uv * vec2(1.0 + uVigRound * 0.5,
                                       1.0 - uVigRound * 0.5));
            float falloff = smoothstep(uVigMid - uVigFeather * 0.5,
                                       uVigMid + uVigFeather * 0.5, r);
            float factor = 1.0 + uVigAmount * falloff;
            color.rgb = clamp(color.rgb * factor, 0.0, 1.0);
        }

        // US-FEAT-B: LUT 3D-texture blend — sample 3D LUT and mix with intensity
        if (uLutEnabled) {
            vec3 lutColor = texture(uLut3D, clamp(color, 0.0, 1.0)).rgb;
            color = mix(color, lutColor, uLutIntensity);
        }

        // US-CG-4: Hue vs Saturation curve. Sampled AFTER the .cube LUT so
        // the curve operates on the final graded colour. The 256x1 R-only
        // LUT is keyed on the HSL hue and the returned scalar multiplies
        // the saturation channel before converting back to RGB. Disabled
        // (uHueVsSatEnabled=false) is a free no-op.
        if (uHueVsSatEnabled) {
            vec3 hsl = rgbToHsl(clamp(color.rgb, 0.0, 1.0));
            // LUT stores satMul/2 as R8_UNorm so the [0..2] range fits
            // into [0..1]; un-pack with *2.0 to restore the multiplier.
            float satMul = texture(uHueVsSatLut, vec2(hsl.x, 0.5)).r * 2.0;
            hsl.y = clamp(hsl.y * satMul, 0.0, 1.0);
            color.rgb = hslToRgb(hsl);
        }

        // US-EF-2: Mask Animation wrap. Compute the mask weight from
        // vTexCoord and mix the ungraded snapshot with the freshly graded
        // colour. uMaskEnabled=false skips the entire stage so the output
        // remains bit-identical to the pre-wrap pipeline.
        if (uMaskEnabled) {
            vec2 uv = vTexCoord;
            vec2 m = (uv - uMaskRect.xy) / max(uMaskRect.zw, vec2(0.0001));
            float weight = 0.0;
            if (uMaskEllipse) {
                vec2 c = m - vec2(0.5);
                float r = length(c);
                float halfRange = max(uMaskFeather * 0.5, 0.001);
                weight = 1.0 - smoothstep(0.5 - halfRange,
                                          0.5 + halfRange, r);
            } else {
                // Rect: edge softness on each side. Inside the rect both
                // factors approach 1; outside, both go to 0. Soft edges
                // stretch by uMaskFeather measured in mask-space units.
                float f = max(uMaskFeather, 0.001);
                vec2 e = smoothstep(0.0, f, m)
                       * (1.0 - smoothstep(1.0 - f, 1.0, m));
                weight = clamp(e.x * e.y, 0.0, 1.0);
            }
            if (uMaskInvert) weight = 1.0 - weight;
            color = mix(ungraded, color, weight);
        }
    }

    if (uFxSepiaEnable)    color = fxApplySepia(color, uFxSepiaStrength);
    if (uFxGrayEnable)     color = fxApplyGray(color, uFxGrayStrength);
    if (uFxInvertEnable)   color = fxApplyInvert(color, uFxInvertStrength);
    if (uFxVignetteEnable) color = fxApplyVignette(color, sampleUv);
    if (uFxNoiseEnable)    color = fxApplyNoise(color, sampleUv);
    if (uFxSharpenEnable)  color = applySharpen(uTexture, sampleUv, 1.0 / uFxTexSize, uFxSharpenAmount);

    // US-EF-4: POST Blur — single-pass 3x3 box-average approximation. Cheap
    // identity at uBlurRadius<0.5 (skips the gather entirely). Uses the
    // lens-distorted sampleUv so the blur kernel sits in the same image
    // space the rest of the shader sampled from.
    if (uBlurRadius > 0.5) {
        vec2 px = uBlurRadius / vec2(textureSize(uTexture, 0));
        vec3 b = vec3(0.0);
        b += texture(uTexture, sampleUv + vec2(-px.x, -px.y)).rgb;
        b += texture(uTexture, sampleUv + vec2( 0.0, -px.y)).rgb;
        b += texture(uTexture, sampleUv + vec2( px.x, -px.y)).rgb;
        b += texture(uTexture, sampleUv + vec2(-px.x,  0.0)).rgb;
        b += texture(uTexture, sampleUv).rgb;
        b += texture(uTexture, sampleUv + vec2( px.x,  0.0)).rgb;
        b += texture(uTexture, sampleUv + vec2(-px.x,  px.y)).rgb;
        b += texture(uTexture, sampleUv + vec2( 0.0,  px.y)).rgb;
        b += texture(uTexture, sampleUv + vec2( px.x,  px.y)).rgb;
        color = mix(color, b / 9.0, smoothstep(0.0, 50.0, uBlurRadius));
    }

    // US-EF-4: POST Sharpen (unsharp mask). Build a 3x3 box average of the
    // lens-distorted neighborhood and add the high-frequency residual back
    // to the post-grade `color`. Identity at uSharpenAmount<0.001 (skip the
    // 9 samples entirely). Slider scale is /100 so the user-visible 0..200
    // maps to a 0..2 multiplier of the residual.
    if (uSharpenAmount > 0.001) {
        vec2 px = 1.0 / vec2(textureSize(uTexture, 0));
        vec3 blur = vec3(0.0);
        blur += texture(uTexture, sampleUv + vec2(-px.x, -px.y)).rgb;
        blur += texture(uTexture, sampleUv + vec2( 0.0, -px.y)).rgb;
        blur += texture(uTexture, sampleUv + vec2( px.x, -px.y)).rgb;
        blur += texture(uTexture, sampleUv + vec2(-px.x,  0.0)).rgb;
        blur += texture(uTexture, sampleUv).rgb;
        blur += texture(uTexture, sampleUv + vec2( px.x,  0.0)).rgb;
        blur += texture(uTexture, sampleUv + vec2(-px.x,  px.y)).rgb;
        blur += texture(uTexture, sampleUv + vec2( 0.0,  px.y)).rgb;
        blur += texture(uTexture, sampleUv + vec2( px.x,  px.y)).rgb;
        blur /= 9.0;
        vec3 baseRgb = texture(uTexture, sampleUv).rgb;
        color = clamp(color + uSharpenAmount * 0.01 * (baseRgb - blur), 0.0, 1.0);
    }

    vec4 outColor = vec4(clamp(color, 0.0, 1.0), texColor.a);
    if (uFxChromaKeyEnable)
        outColor = applyChromaKey(outColor.rgb, uFxChromaKey, uFxChromaTolerance);

    outColor *= uClipOpacity;
    FragColor = outColor;
}
)";

// Section C — NV12 zero-copy preview. We sample two single-component textures
// (Y as R8, UV as RG8) backed by the same FFmpeg-decoded ID3D11Texture2D
// subresource and convert to BT.709 limited-range RGB on the GPU.
static const char *nv12VertexShaderSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

static const char *nv12FragmentShaderSrc = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexY;
uniform sampler2D uTexUV;
void main() {
    float y = (texture(uTexY, vTexCoord).r - 0.0625) * 1.164;
    vec2 cb_cr = texture(uTexUV, vTexCoord).rg - vec2(0.5, 0.5);
    float r = y                  + 1.793 * cb_cr.y;
    float g = y - 0.213 * cb_cr.x - 0.533 * cb_cr.y;
    float b = y + 2.112 * cb_cr.x;
    FragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
)";

#if defined(Q_OS_WIN)
namespace {
struct NV12RegisteredTex {
    void *d3d11Tex = nullptr;
    int   subresource = 0;
    HANDLE hY = nullptr;
    HANDLE hUV = nullptr;
    GLuint glTexY = 0;
    GLuint glTexUV = 0;
    int    width = 0;
    int    height = 0;
};

struct CacheKey {
    const GLPreview *owner;
    void *d3d11Tex;
    int   subresource;
    bool operator==(const CacheKey &o) const noexcept {
        return owner == o.owner && d3d11Tex == o.d3d11Tex && subresource == o.subresource;
    }
};
struct CacheKeyHash {
    size_t operator()(const CacheKey &k) const noexcept {
        // Mix all three identity components — owner discriminates instances,
        // tex+subresource discriminate frames within an instance.
        const auto a = reinterpret_cast<uintptr_t>(k.owner);
        const auto b = reinterpret_cast<uintptr_t>(k.d3d11Tex);
        return std::hash<uintptr_t>{}(a) ^ (std::hash<uintptr_t>{}(b) << 1)
               ^ (std::hash<int>{}(k.subresource) << 2);
    }
};

std::unordered_map<CacheKey, NV12RegisteredTex, CacheKeyHash> &nv12Cache()
{
    static std::unordered_map<CacheKey, NV12RegisteredTex, CacheKeyHash> g;
    return g;
}
} // namespace
#endif // Q_OS_WIN

GLPreview::GLPreview(QWidget *parent)
    : QOpenGLWidget(parent), m_vbo(QOpenGLBuffer::VertexBuffer)
{
    setMinimumSize(320, 180);
    m_textToolCaretTimer.setInterval(500);
    connect(&m_textToolCaretTimer, &QTimer::timeout, this, [this]() {
        m_textToolCaretVisible = !m_textToolCaretVisible;
        update();
    });
}

void GLPreview::setBrushAnimation(BrushAnimation *animation)
{
    if (m_brushAnimation == animation)
        return;
    m_brushAnimation = animation;
    m_needsUpload = true;
    update();
}

void GLPreview::clearBrushAnimation()
{
    m_brushAnimation = nullptr;
    m_brushAnimationProgress = 0.0;
    m_needsUpload = true;
    update();
}

void GLPreview::setBrushAnimationProgress(double progress)
{
    const double clamped = qBound(0.0, progress, 1.0);
    if (std::abs(m_brushAnimationProgress - clamped) <= 1.0e-6)
        return;
    m_brushAnimationProgress = clamped;
    m_needsUpload = true;
    update();
}

GLPreview::~GLPreview()
{
    // Primary cleanup path is cleanupGL() via QOpenGLContext::aboutToBeDestroyed.
    // Fallback: only touch GL state here if a current context is available — calling
    // makeCurrent() on a destroyed context segfaults some drivers on shutdown.
    if (QOpenGLContext::currentContext() || context()) {
        cleanupGL();
    } else {
        // Context already gone — clear raw pointers to skip double-free, but don't
        // touch GL state.
        m_texture = nullptr;
        m_program = nullptr;
    }
}

void GLPreview::cleanupGL()
{
    if (!context())
        return;

    makeCurrent();
    releaseRegisteredTexturesLocked();
#if defined(Q_OS_WIN)
    if (m_interopDevice) {
        if (gWglDXCloseDeviceNV)
            gWglDXCloseDeviceNV(static_cast<HANDLE>(m_interopDevice));
        m_interopDevice = nullptr;
        m_currentInteropD3D11Device = nullptr;
    }
#endif
    if (m_nv12Program) {
        delete m_nv12Program;
        m_nv12Program = nullptr;
    }
    if (m_lutTexture) {
        delete m_lutTexture;
        m_lutTexture = nullptr;
    }
    if (m_curveLutTex) {
        delete m_curveLutTex;
        m_curveLutTex = nullptr;
    }
    if (m_hueVsSatTex) {
        delete m_hueVsSatTex;
        m_hueVsSatTex = nullptr;
    }
    if (m_texture) {
        delete m_texture;
        m_texture = nullptr;
    }
    if (m_program) {
        delete m_program;
        m_program = nullptr;
    }
    if (m_vbo.isCreated())
        m_vbo.destroy();
    if (m_vao.isCreated())
        m_vao.destroy();
    doneCurrent();
}

void GLPreview::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    // Qt recommends cleaning up GL resources when the context is about to be
    // destroyed rather than in the widget destructor (Qt 5/6 QOpenGLWidget docs).
    if (auto *ctx = context()) {
        connect(ctx, &QOpenGLContext::aboutToBeDestroyed,
                this, &GLPreview::cleanupGL, Qt::UniqueConnection);
    }

    createShaderProgram();

    // Fullscreen quad: position(x,y) + texcoord(u,v)
    float vertices[] = {
        // pos        // tex
        -1.0f,  1.0f,  0.0f, 0.0f,  // top-left
        -1.0f, -1.0f,  0.0f, 1.0f,  // bottom-left
         1.0f,  1.0f,  1.0f, 0.0f,  // top-right
         1.0f, -1.0f,  1.0f, 1.0f,  // bottom-right
    };

    m_vao.create();
    m_vao.bind();

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(vertices, sizeof(vertices));

    // Position attribute
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    // TexCoord attribute
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    m_vbo.release();
    m_vao.release();

    detectInteropExtension();
}

void GLPreview::detectInteropExtension()
{
    m_interopAvailable = false;

#if defined(Q_OS_WIN)
    if (!veditorGlInteropEnabled()) {
        qInfo() << "[interop] disabled (envvar VEDITOR_GL_INTEROP not set)";
        return;
    }

    QOpenGLContext *ctx = context();
    if (!ctx) {
        qInfo() << "[interop] unavailable (no current QOpenGLContext)";
        return;
    }

    auto getExtensionsString = reinterpret_cast<PFNWGLGETEXTENSIONSSTRINGARBPROC>(
        ctx->getProcAddress("wglGetExtensionsStringARB"));
    if (!getExtensionsString) {
        qInfo() << "[interop] unavailable (wglGetExtensionsStringARB missing)";
        return;
    }

    HDC hdc = wglGetCurrentDC();
    if (!hdc) {
        qInfo() << "[interop] unavailable (wglGetCurrentDC returned null)";
        return;
    }

    const char *exts = getExtensionsString(hdc);
    if (!exts) {
        qInfo() << "[interop] unavailable (wglGetExtensionsStringARB returned null)";
        return;
    }

    // Match exact token to avoid matching WGL_NV_DX_interop (v1, no multi-thread
    // lock) or hypothetical WGL_NV_DX_interop2_extended. Leading match substitutes
    // a space sentinel for the missing predecessor; trailing must be space or NUL.
    const char *needle = "WGL_NV_DX_interop2";
    bool found = false;
    for (const char *p = std::strstr(exts, needle); p; p = std::strstr(p + 1, needle)) {
        const char before = (p == exts) ? ' ' : *(p - 1);
        const char after  = *(p + std::strlen(needle));
        if (before == ' ' && (after == ' ' || after == '\0')) {
            found = true;
            break;
        }
    }
    if (!found) {
        qInfo() << "[interop] unavailable (WGL_NV_DX_interop2 not in extension string)";
        return;
    }

    struct ProcEntry { const char *name; void **slot; };
    const ProcEntry procs[] = {
        { "wglDXOpenDeviceNV",       reinterpret_cast<void**>(&gWglDXOpenDeviceNV)       },
        { "wglDXCloseDeviceNV",      reinterpret_cast<void**>(&gWglDXCloseDeviceNV)      },
        { "wglDXRegisterObjectNV",   reinterpret_cast<void**>(&gWglDXRegisterObjectNV)   },
        { "wglDXUnregisterObjectNV", reinterpret_cast<void**>(&gWglDXUnregisterObjectNV) },
        { "wglDXLockObjectsNV",      reinterpret_cast<void**>(&gWglDXLockObjectsNV)      },
        { "wglDXUnlockObjectsNV",    reinterpret_cast<void**>(&gWglDXUnlockObjectsNV)    },
    };
    for (const ProcEntry &e : procs) {
        *e.slot = reinterpret_cast<void*>(ctx->getProcAddress(e.name));
        if (!*e.slot) {
            qInfo() << "[interop] unavailable (proc resolution failed:" << e.name << ")";
            return;
        }
    }

    m_interopAvailable = true;
    const char *renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    qInfo() << "[interop] available (WGL_NV_DX_interop2, GL_RENDERER="
            << (renderer ? renderer : "<unknown>") << ")";
#else
    qInfo() << "[interop] disabled (non-Windows platform)";
#endif
}

void GLPreview::setSharedD3D11Device(void *d3d11Device)
{
#if defined(Q_OS_WIN)
    // Store the pointer regardless of m_interopAvailable — VideoPlayer can
    // call us before initializeGL runs (loadFile races the first paint).
    // Lazy ensureInteropDeviceForPaint gates the actual open by detection.
    if (d3d11Device == m_pendingD3D11Device)
        return;
    m_pendingD3D11Device = d3d11Device;
    // Existing interop handle was opened against the previous device — drop
    // every cached register entry plus the device handle itself before the
    // next paint reopens against the new device. close on the GL thread.
    if (m_interopDevice && d3d11Device != m_currentInteropD3D11Device) {
        if (context() && QOpenGLContext::currentContext() != context()) {
            makeCurrent();
            releaseRegisteredTexturesLocked();
            if (gWglDXCloseDeviceNV)
                gWglDXCloseDeviceNV(static_cast<HANDLE>(m_interopDevice));
            doneCurrent();
        } else {
            releaseRegisteredTexturesLocked();
            if (gWglDXCloseDeviceNV)
                gWglDXCloseDeviceNV(static_cast<HANDLE>(m_interopDevice));
        }
        m_interopDevice = nullptr;
        m_currentInteropD3D11Device = nullptr;
    }
    update();
#else
    Q_UNUSED(d3d11Device);
#endif
}

void GLPreview::displayD3D11Frame(void *d3d11Texture, int subresource, int width, int height)
{
#if defined(Q_OS_WIN)
    if (!m_interopAvailable || !d3d11Texture || width <= 0 || height <= 0)
        return;
    m_pendingD3D11Texture = d3d11Texture;
    m_pendingD3D11Subresource = subresource;
    m_pendingD3D11Width = width;
    m_pendingD3D11Height = height;
    update();
#else
    Q_UNUSED(d3d11Texture);
    Q_UNUSED(subresource);
    Q_UNUSED(width);
    Q_UNUSED(height);
#endif
}

void GLPreview::flushInteropCache()
{
#if defined(Q_OS_WIN)
    if (!context())
        return;
    makeCurrent();
    releaseRegisteredTexturesLocked();
    doneCurrent();
    m_pendingD3D11Texture = nullptr;
#endif
}

bool GLPreview::ensureInteropDeviceForPaint()
{
#if defined(Q_OS_WIN)
    if (!m_interopAvailable)
        return false;
    if (m_interopDevice)
        return true;
    if (!m_pendingD3D11Device)
        return false;
    if (!gWglDXOpenDeviceNV)
        return false;
    HANDLE h = gWglDXOpenDeviceNV(m_pendingD3D11Device);
    if (!h) {
        qWarning() << "[interop] device open failed (wglDXOpenDeviceNV returned null) for d3d11Device="
                   << m_pendingD3D11Device;
        return false;
    }
    m_interopDevice = h;
    m_currentInteropD3D11Device = m_pendingD3D11Device;
    qInfo() << "[interop] device opened (d3d11Device=" << m_pendingD3D11Device
            << ", interopHandle=" << h << ")";
    return true;
#else
    return false;
#endif
}

void GLPreview::releaseRegisteredTexturesLocked()
{
#if defined(Q_OS_WIN)
    auto &cache = nv12Cache();
    for (auto it = cache.begin(); it != cache.end(); ) {
        if (it->first.owner != this) { ++it; continue; }
        NV12RegisteredTex &r = it->second;
        if (m_interopDevice && gWglDXUnregisterObjectNV) {
            if (r.hY)  gWglDXUnregisterObjectNV(static_cast<HANDLE>(m_interopDevice), r.hY);
            if (r.hUV) gWglDXUnregisterObjectNV(static_cast<HANDLE>(m_interopDevice), r.hUV);
        }
        if (r.glTexY)  glDeleteTextures(1, &r.glTexY);
        if (r.glTexUV) glDeleteTextures(1, &r.glTexUV);
        it = cache.erase(it);
    }
#endif
}

void GLPreview::createShaderProgram()
{
    m_program = new QOpenGLShaderProgram(this);
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSrc)) {
        qWarning() << "GLPreview: vertex shader compile failed:" << m_program->log();
    }
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSrc)) {
        qWarning() << "GLPreview: fragment shader compile failed:" << m_program->log();
    }
    if (!m_program->link()) {
        qWarning() << "GLPreview: shader link failed:" << m_program->log();
        delete m_program;
        m_program = nullptr;
        return;
    }

    m_locTexture        = m_program->uniformLocation("uTexture");
    m_locBrightness     = m_program->uniformLocation("uBrightness");
    m_locContrast       = m_program->uniformLocation("uContrast");
    m_locSaturation     = m_program->uniformLocation("uSaturation");
    m_locHue            = m_program->uniformLocation("uHue");
    m_locTemperature    = m_program->uniformLocation("uTemperature");
    m_locTint           = m_program->uniformLocation("uTint");
    m_locGamma          = m_program->uniformLocation("uGamma");
    m_locHighlights     = m_program->uniformLocation("uHighlights");
    m_locShadows        = m_program->uniformLocation("uShadows");
    m_locExposure       = m_program->uniformLocation("uExposure");
    m_locEffectsEnabled = m_program->uniformLocation("uEffectsEnabled");
    m_locClipOpacity    = m_program->uniformLocation("uClipOpacity");
    m_locHdrTransfer    = m_program->uniformLocation("uHdrTransfer");

    m_locFxBlurEnable        = m_program->uniformLocation("uFxBlurEnable");
    m_locFxBlurRadius        = m_program->uniformLocation("uFxBlurRadius");
    m_locFxTexSize           = m_program->uniformLocation("uFxTexSize");
    m_locFxNoiseEnable       = m_program->uniformLocation("uFxNoiseEnable");
    m_locFxNoiseAmount       = m_program->uniformLocation("uFxNoiseAmount");
    m_locFxSepiaEnable       = m_program->uniformLocation("uFxSepiaEnable");
    m_locFxSepiaStrength     = m_program->uniformLocation("uFxSepiaStrength");
    m_locFxGrayEnable        = m_program->uniformLocation("uFxGrayEnable");
    m_locFxGrayStrength      = m_program->uniformLocation("uFxGrayStrength");
    m_locFxInvertEnable      = m_program->uniformLocation("uFxInvertEnable");
    m_locFxInvertStrength    = m_program->uniformLocation("uFxInvertStrength");
    m_locFxVignetteEnable    = m_program->uniformLocation("uFxVignetteEnable");
    m_locFxVignetteIntensity = m_program->uniformLocation("uFxVignetteIntensity");
    m_locFxVignetteRadius    = m_program->uniformLocation("uFxVignetteRadius");
    m_locFxSharpenEnable     = m_program->uniformLocation("uFxSharpenEnable");
    m_locFxSharpenAmount     = m_program->uniformLocation("uFxSharpenAmount");
    m_locFxMosaicEnable      = m_program->uniformLocation("uFxMosaicEnable");
    m_locFxMosaicSize        = m_program->uniformLocation("uFxMosaicSize");
    m_locFxChromaKeyEnable   = m_program->uniformLocation("uFxChromaKeyEnable");
    m_locFxChromaKey         = m_program->uniformLocation("uFxChromaKey");
    m_locFxChromaTolerance   = m_program->uniformLocation("uFxChromaTolerance");
    m_locFxTime              = m_program->uniformLocation("uFxTime");

    // Lift/Gamma/Gain (vec4: xyz=RGB, w=Luma)
    m_locLift     = m_program->uniformLocation("uLift");
    m_locLggGamma = m_program->uniformLocation("uLggGamma");
    m_locGain     = m_program->uniformLocation("uGain");

    // LUT
    m_locLut3D         = m_program->uniformLocation("uLut3D");
    m_locLutIntensity  = m_program->uniformLocation("uLutIntensity");
    m_locLutEnabled    = m_program->uniformLocation("uLutEnabled");

    // US-CG-1: RGB curves
    m_locCurveLut       = m_program->uniformLocation("uCurveLut");
    m_locCurvesEnabled  = m_program->uniformLocation("uCurvesEnabled");

    // US-CG-2: White-balance gain triple uniform.
    m_locWb             = m_program->uniformLocation("uWb");

    // US-CG-3: Radial vignette uniforms.
    m_locVigAmount      = m_program->uniformLocation("uVigAmount");
    m_locVigMid         = m_program->uniformLocation("uVigMid");
    m_locVigRound       = m_program->uniformLocation("uVigRound");
    m_locVigFeather     = m_program->uniformLocation("uVigFeather");

    // US-EF-1: Chroma Key uniforms.
    m_locChromaEnabled  = m_program->uniformLocation("uChromaEnabled");
    m_locChromaKeyHsl   = m_program->uniformLocation("uChromaKeyHsl");
    m_locChromaTol      = m_program->uniformLocation("uChromaTol");
    m_locChromaSpill    = m_program->uniformLocation("uChromaSpill");
    m_locChromaSoft     = m_program->uniformLocation("uChromaSoft");

    // US-EF-3: HSL Qualifier uniforms.
    m_locHslqEnabled    = m_program->uniformLocation("uHslqEnabled");
    m_locHslqHueCenter  = m_program->uniformLocation("uHslqHueCenter");
    m_locHslqHueRange   = m_program->uniformLocation("uHslqHueRange");
    m_locHslqSatRange   = m_program->uniformLocation("uHslqSatRange");
    m_locHslqLumaRange  = m_program->uniformLocation("uHslqLumaRange");
    m_locHslqSoftness   = m_program->uniformLocation("uHslqSoftness");
    m_locHslqLift       = m_program->uniformLocation("uHslqLift");
    m_locHslqGamma      = m_program->uniformLocation("uHslqGamma");
    m_locHslqGain       = m_program->uniformLocation("uHslqGain");

    // US-CG-4: Hue vs Saturation curve uniform locations.
    m_locHueVsSatEnabled = m_program->uniformLocation("uHueVsSatEnabled");
    m_locHueVsSatLut     = m_program->uniformLocation("uHueVsSatLut");

    // US-EF-2: Mask Animation uniforms.
    m_locMaskEnabled    = m_program->uniformLocation("uMaskEnabled");
    m_locMaskEllipse    = m_program->uniformLocation("uMaskEllipse");
    m_locMaskInvert     = m_program->uniformLocation("uMaskInvert");
    m_locMaskFeather    = m_program->uniformLocation("uMaskFeather");
    m_locMaskRect       = m_program->uniformLocation("uMaskRect");

    // US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens Distortion.
    // All three default to identity (0) so the stages are free no-ops until
    // the user touches the エフェクト sliders in ColorGradingPanel.
    m_locSharpenAmount  = m_program->uniformLocation("uSharpenAmount");
    m_locBlurRadius     = m_program->uniformLocation("uBlurRadius");
    m_locLensDistortion = m_program->uniformLocation("uLensDistortion");

    // US-3D: 3-axis rotation + perspective foreshortening. Identity matrix
    // (uRot3DEnabled=false) is a free no-op — the shader skips the warp.
    m_locRot3DEnabled    = m_program->uniformLocation("uRot3DEnabled");
    m_locRot3D           = m_program->uniformLocation("uRot3D");
    m_locPerspectiveDist = m_program->uniformLocation("uPerspectiveDist");

    // US-INT-4: stabilizer pre-warp uniform locations.
    m_locStabEnabled = m_program->uniformLocation("uStabEnabled");
    m_locStab        = m_program->uniformLocation("uStab");

    // NV12 zero-copy program — only used when the interop fast path engages.
    // Failure to compile/link is non-fatal: callers fall back to the legacy
    // QImage path automatically.
    m_nv12Program = new QOpenGLShaderProgram(this);
    if (!m_nv12Program->addShaderFromSourceCode(QOpenGLShader::Vertex, nv12VertexShaderSrc)
        || !m_nv12Program->addShaderFromSourceCode(QOpenGLShader::Fragment, nv12FragmentShaderSrc)
        || !m_nv12Program->link()) {
        qWarning() << "GLPreview: NV12 shader build failed:" << m_nv12Program->log();
        delete m_nv12Program;
        m_nv12Program = nullptr;
    } else {
        m_locNv12TexY  = m_nv12Program->uniformLocation("uTexY");
        m_locNv12TexUV = m_nv12Program->uniformLocation("uTexUV");
    }
}

void GLPreview::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void GLPreview::displayFrame(const QImage &frame)
{
    if (frame.isNull()) {
        qWarning() << "GLPreview::displayFrame called with null image";
        return;
    }
    // US-INT-4: cache the source-frame dimensions so the stabilizer matrix
    // builder can convert SOURCE-pixel offsets to UV-fraction. Identity
    // path (m_stabKeyframes empty) leaves the matrix at identity so this
    // is a free no-op when stabilization is not in use.
    m_stabFrameW = frame.width();
    m_stabFrameH = frame.height();
    const QImage::Format inFmt = frame.format();
    const bool is16 = (inFmt == QImage::Format_RGBA64
                       || inFmt == QImage::Format_RGBA64_Premultiplied);
    // Format hot path: SDR sources from frameToImage arrive as
    // Format_RGB888 already. Stamping that straight into m_currentFrame
    // saves an entire RGBA8888 conversion per frame (~3.7 ms for a 1440p
    // frame on this hardware) at no GPU cost — paintGL uploads as
    // QOpenGLTexture::RGB / RGB8_UNorm and the shader's texture() lookup
    // returns vec4(r,g,b,1.0) per the GL spec, identical to RGBA sampling.
    //
    // Phase 1e Win #8: same trick for the multi-track compositor output,
    // which arrives as ARGB32_Premultiplied. Qt stores that format as
    // BGRA bytes on little-endian; uploading via GL_BGRA + GL_UNSIGNED_BYTE
    // saves the per-pixel byte swap + alpha-unpremul that
    // convertToFormat(RGBA8888) costs (~3-5 ms / 1080p tick). Compositor
    // output has alpha=1.0 everywhere (black-fill base + opaque overlay
    // SourceOver), so premul=straight and the fragment shader's
    // texture().rgb sampling produces the same visible pixels. Disable
    // path: VEDITOR_BGRA_UPLOAD_DISABLE=1.
    //
    // INVARIANT (architect NIT-1): the alpha=1.0 invariant relies on
    // overlay layer pixels themselves being opaque (alpha=1.0). The
    // current decode pipeline emits RGB24 frames (no alpha), which
    // QPainter promotes to alpha=0xFF when drawing — preserving the
    // invariant. If a future overlay format introduces non-opaque
    // pixels (transparent PNG / sticker), this fast path's premul=
    // straight assumption breaks for those pixels and Win #8 must
    // gate on an opacity check or fall back to RGBA8888.
    static const bool bgraUploadEnabled =
        qEnvironmentVariableIntValue("VEDITOR_BGRA_UPLOAD_DISABLE") == 0;
    if (is16) {
        m_currentFrame = (inFmt == QImage::Format_RGBA64)
            ? frame
            : frame.convertToFormat(QImage::Format_RGBA64);
    } else if (inFmt == QImage::Format_RGB888) {
        m_currentFrame = frame;
    } else if (bgraUploadEnabled && inFmt == QImage::Format_ARGB32_Premultiplied) {
        m_currentFrame = frame;
    } else {
        m_currentFrame = frame.convertToFormat(QImage::Format_RGBA8888);
    }
    if (m_currentFrame.isNull()) {
        qWarning() << "GLPreview: convertToFormat returned null";
        return;
    }
    if (m_displayAspectRatio <= 0.0 && m_currentFrame.height() > 0)
        m_displayAspectRatio = static_cast<double>(m_currentFrame.width()) / m_currentFrame.height();
    m_needsUpload = true;
    update();
}

void GLPreview::setDisplayAspectRatio(double aspectRatio)
{
    m_displayAspectRatio = (aspectRatio > 0.0) ? aspectRatio : 0.0;
    update();
}

void GLPreview::setColorCorrection(const ColorCorrection &cc)
{
    m_cc = cc;
    update();
}

void GLPreview::setHdrTransfer(int transfer)
{
    const int clamped = (transfer == 1 || transfer == 2) ? transfer : 0;
    if (m_hdrTransfer == clamped)
        return;
    m_hdrTransfer = clamped;
    update();
}

void GLPreview::setVideoEffects(const QVector<VideoEffect> &effects)
{
    m_videoEffects = effects;
    update();
}

void GLPreview::renderPendingD3D11Frame()
{
#if defined(Q_OS_WIN)
    if (!m_pendingD3D11Texture)
        return;

    void *d3d11Tex = m_pendingD3D11Texture;
    int subresource = m_pendingD3D11Subresource;
    int frameW = m_pendingD3D11Width;
    int frameH = m_pendingD3D11Height;
    // Always clear pending so a failed-to-register frame does not retry forever.
    m_pendingD3D11Texture = nullptr;

    if (!m_nv12Program || !ensureInteropDeviceForPaint())
        return;
    if (!gWglDXRegisterObjectNV || !gWglDXLockObjectsNV || !gWglDXUnlockObjectsNV)
        return;

    auto &cache = nv12Cache();
    CacheKey key{this, d3d11Tex, subresource};
    auto it = cache.find(key);
    if (it == cache.end()) {
        NV12RegisteredTex r;
        r.d3d11Tex = d3d11Tex;
        r.subresource = subresource;
        r.width = frameW;
        r.height = frameH;
        glGenTextures(1, &r.glTexY);
        glGenTextures(1, &r.glTexUV);
        // Allocate format-specific storage on the GL side BEFORE registering —
        // WGL_NV_DX_interop2 requires the GL texture's internal format to
        // match what the driver will expose. NV12 maps cleanly to Y as R8
        // (full WxH) and UV interleaved as RG8 (W/2 x H/2). Without
        // glTexImage2D the driver cannot resolve the plane mapping and
        // wglDXRegisterObjectNV either fails or returns aliased handles.
        glBindTexture(GL_TEXTURE_2D, r.glTexY);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, frameW, frameH,
                     0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, r.glTexUV);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, frameW / 2, frameH / 2,
                     0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        r.hY = gWglDXRegisterObjectNV(static_cast<HANDLE>(m_interopDevice),
                                      d3d11Tex, r.glTexY,
                                      GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        if (!r.hY) {
            static bool warnedY = false;
            if (!warnedY) {
                qWarning() << "[interop] register Y plane failed for d3d11Tex=" << d3d11Tex
                           << "subres=" << subresource
                           << "— falling back to QImage path for this frame";
                warnedY = true;
            }
            glDeleteTextures(1, &r.glTexY);
            glDeleteTextures(1, &r.glTexUV);
            return;
        }
        r.hUV = gWglDXRegisterObjectNV(static_cast<HANDLE>(m_interopDevice),
                                       d3d11Tex, r.glTexUV,
                                       GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        if (!r.hUV) {
            static bool warnedUV = false;
            if (!warnedUV) {
                qWarning() << "[interop] register UV plane failed for d3d11Tex=" << d3d11Tex
                           << "subres=" << subresource
                           << "— falling back to QImage path for this frame";
                warnedUV = true;
            }
            if (gWglDXUnregisterObjectNV)
                gWglDXUnregisterObjectNV(static_cast<HANDLE>(m_interopDevice), r.hY);
            glDeleteTextures(1, &r.glTexY);
            glDeleteTextures(1, &r.glTexUV);
            return;
        }
        auto inserted = cache.emplace(key, r);
        it = inserted.first;
    }

    NV12RegisteredTex &r = it->second;

    HANDLE handles[2] = { r.hY, r.hUV };
    // Phase 1e Win #11 stall trace — wglDXLockObjectsNV is a CPU↔GPU
    // sync barrier; under multi-track AV1 1440p decode + screen capture
    // adapter contention this can block the GL render thread for 1–2 s
    // without triggering any decode-side probe. Default off.
    const bool traceStall = stallTraceEnabled();
    QElapsedTimer interopLockTimer;
    if (traceStall)
        interopLockTimer.start();
    if (!gWglDXLockObjectsNV(static_cast<HANDLE>(m_interopDevice), 2, handles)) {
        static bool warnedLock = false;
        if (!warnedLock) {
            qWarning() << "[interop] wglDXLockObjectsNV failed — falling back this frame";
            warnedLock = true;
        }
        return;
    }
    if (traceStall && interopLockTimer.isValid()) {
        const qint64 elapsedMs = interopLockTimer.elapsed();
        if (elapsedMs >= kStallThresholdInteropLockMs) {
            qWarning().noquote()
                << QStringLiteral("[stall>=%1ms] wglDXLockObjectsNV %2ms")
                       .arg(kStallThresholdInteropLockMs)
                       .arg(elapsedMs);
        }
    }

    // Letterbox the NV12 fast path the same way the legacy path does so
    // viewport math stays consistent with displayAspectRatio.
    const qreal dpr = devicePixelRatioF();
    const int physW = qMax(1, qRound(width() * dpr));
    const int physH = qMax(1, qRound(height() * dpr));
    const double frameAspect =
        (m_displayAspectRatio > 0.0 && std::isfinite(m_displayAspectRatio))
            ? m_displayAspectRatio
            : (r.height > 0 ? static_cast<double>(r.width) / r.height : 1.0);
    const double widgetAspect = physH > 0 ? static_cast<double>(physW) / physH : frameAspect;
    int viewportX = 0, viewportY = 0, viewportW = physW, viewportH = physH;
    if (frameAspect > 0.0 && widgetAspect > 0.0) {
        if (widgetAspect > frameAspect) {
            viewportW = qMax(1, qRound(physH * frameAspect));
            viewportX = (physW - viewportW) / 2;
        } else {
            viewportH = qMax(1, qRound(physW / frameAspect));
            viewportY = (physH - viewportH) / 2;
        }
    }
    glViewport(viewportX, viewportY, viewportW, viewportH);

    m_nv12Program->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r.glTexY);
    m_nv12Program->setUniformValue(m_locNv12TexY, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r.glTexUV);
    m_nv12Program->setUniformValue(m_locNv12TexUV, 1);
    m_vao.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();
    m_nv12Program->release();

    gWglDXUnlockObjectsNV(static_cast<HANDLE>(m_interopDevice), 2, handles);
#endif
}

void GLPreview::paintGL()
{
    static int paintCount = 0;
    if (++paintCount <= 5 || (paintCount % 100) == 0) {
        qInfo() << "GLPreview::paintGL #" << paintCount
                << "widget(logical)=" << width() << "x" << height()
                << "dpr=" << devicePixelRatioF()
                << "frame=" << m_currentFrame.width() << "x" << m_currentFrame.height()
                << "upload=" << m_needsUpload;
    }

    glClear(GL_COLOR_BUFFER_BIT);

#if defined(Q_OS_WIN)
    // Lazy-open the interop device the first paint after VideoPlayer hands
    // us a D3D11 device — eager open would race initializeGL.
    if (m_interopAvailable && m_pendingD3D11Device && !m_interopDevice)
        ensureInteropDeviceForPaint();

    if (m_pendingD3D11Texture && m_interopAvailable) {
        renderPendingD3D11Frame();
        return;
    }
#endif

    if (m_currentFrame.isNull()) return;

    // glViewport expects PHYSICAL pixels, but QWidget::width()/height() are
    // LOGICAL (device-independent) pixels. On a high-DPI display with DPR=1.5
    // or 2.0, using logical coordinates makes the video render in a fraction
    // of the widget — which is what the "small video in big panel" bug was.
    const qreal dpr = devicePixelRatioF();
    const int physW = qMax(1, qRound(width() * dpr));
    const int physH = qMax(1, qRound(height() * dpr));

    const double frameAspect =
        (m_displayAspectRatio > 0.0 && std::isfinite(m_displayAspectRatio))
            ? m_displayAspectRatio
            : ((m_currentFrame.height() > 0)
                   ? static_cast<double>(m_currentFrame.width()) / m_currentFrame.height()
                   : 1.0);
    const double widgetAspect =
        (physH > 0) ? static_cast<double>(physW) / physH : frameAspect;

    int viewportX = 0;
    int viewportY = 0;
    int viewportW = physW;
    int viewportH = physH;

    if (frameAspect > 0.0 && widgetAspect > 0.0) {
        if (widgetAspect > frameAspect) {
            viewportW = qMax(1, qRound(physH * frameAspect));
            viewportX = (physW - viewportW) / 2;
        } else {
            viewportH = qMax(1, qRound(physW / frameAspect));
            viewportY = (physH - viewportH) / 2;
        }
    }

    PreviewClipMotion previewMotion;
    if (m_timeline) {
        double timelineSec = 0.0;
        if (auto *player = qobject_cast<VideoPlayer *>(parentWidget()))
            timelineSec = static_cast<double>(player->timelinePositionUs()) / AV_TIME_BASE;
        previewMotion = resolvePreviewClipMotion(m_timeline, timelineSec,
                                                 m_compositeBakedMode,
                                                 m_videoSourceScale,
                                                 m_videoSourceDx,
                                                 m_videoSourceDy);
    }

    double renderScale = m_videoSourceScale;
    double renderDx = m_videoSourceDx;
    double renderDy = m_videoSourceDy;
    double clipOpacity = 1.0;
    QImage uploadFrame = m_currentFrame;
    if (m_brushAnimation) {
        OverlayRenderer::renderBrushOverlay(uploadFrame,
                                            m_brushAnimation,
                                            m_brushAnimationProgress);
    }
    if (previewMotion.valid) {
        renderScale = previewMotion.scale;
        renderDx = previewMotion.dx;
        renderDy = previewMotion.dy;
        clipOpacity = qBound(0.0, previewMotion.opacity, 1.0);
        if (m_videoDragMode == VideoDragNone) {
            m_videoSourceScale = renderScale;
            m_videoSourceDx = renderDx;
            m_videoSourceDy = renderDy;
        }
        if (!m_compositeBakedMode
            && (std::abs(previewMotion.rotation2D) > 1e-4 || previewMotion.is3DLayer)) {
            uploadFrame = applyPlanarRotation(uploadFrame, previewMotion.rotation2D);
            if (previewMotion.is3DLayer) {
                Camera3DState cameraState;
                uploadFrame = Camera3D::applyPerspective(uploadFrame,
                                                         previewMotion.layer3D,
                                                         cameraState,
                                                         uploadFrame.size());
            }
        }
    }

    // US-T34 OBS-style source transform — shrink/move the viewport so the
    // same texture renders inside a translated+scaled sub-rect of the
    // letterbox. OpenGL's Y axis is bottom-up, so dy is inverted.
    // Skip the viewport transform when the multi-track compositor has
    // already baked per-clip scale/dx/dy into the canvas image. Without
    // this guard, the per-tick composite pass would either clobber the
    // user's drag state (if it called setVideoSourceTransform(1, 0, 0))
    // or apply the transform twice on top of the baked canvas.
    if (!m_compositeBakedMode
        && (renderScale != 1.0 || renderDx != 0.0 || renderDy != 0.0)) {
        const int baseW = viewportW;
        const int baseH = viewportH;
        const int newW = qMax(1, qRound(baseW * renderScale));
        const int newH = qMax(1, qRound(baseH * renderScale));
        const int baseCx = viewportX + baseW / 2;
        const int baseCy = viewportY + baseH / 2;
        const int offsetPxX = qRound(renderDx * baseW);
        const int offsetPxY = qRound(renderDy * baseH);
        viewportX = baseCx + offsetPxX - newW / 2;
        viewportY = baseCy - offsetPxY - newH / 2;
        viewportW = newW;
        viewportH = newH;
    }

    // Upload texture if new frame.
    //
    // Re-use a single QOpenGLTexture across frames — allocating a new texture
    // per frame (~8 MB for 1080p RGBA) thrashes driver memory and has been
    // observed to crash Intel/AMD drivers after a few hundred frames.
    if (m_needsUpload) {
        const int fw = uploadFrame.width();
        const int fh = uploadFrame.height();
        if (fw <= 0 || fh <= 0) {
            m_needsUpload = false;
            return;
        }

        const QImage::Format frameFmt = uploadFrame.format();
        const bool is16 = (frameFmt == QImage::Format_RGBA64
                           || frameFmt == QImage::Format_RGBA64_Premultiplied);
        const bool isRGB888 = (frameFmt == QImage::Format_RGB888);
        // Phase 1e Win #8: ARGB32_Premultiplied bytes are BGRA on
        // little-endian. Upload via GL_BGRA so the GL driver swizzles
        // into the RGBA8 internal format, skipping a CPU-side
        // convertToFormat(RGBA8888). Compositor output is always alpha=1.0
        // so premul vs straight produces identical visible pixels through
        // the existing fragment shader (texture().rgb sampling).
        const bool isARGB32Premul = (frameFmt == QImage::Format_ARGB32_Premultiplied);

        const bool sizeChanged = !m_texture
            || m_texture->width() != fw
            || m_texture->height() != fh
            || m_textureFormat != frameFmt;

        if (sizeChanged) {
            if (m_texture) {
                delete m_texture;
                m_texture = nullptr;
            }
            m_texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
            m_texture->setSize(fw, fh);
            QOpenGLTexture::TextureFormat texFormat;
            QOpenGLTexture::PixelFormat pixFormat;
            QOpenGLTexture::PixelType pixType;
            if (is16) {
                texFormat = QOpenGLTexture::RGBA16_UNorm;
                pixFormat = QOpenGLTexture::RGBA;
                pixType   = QOpenGLTexture::UInt16;
            } else if (isRGB888) {
                texFormat = QOpenGLTexture::RGB8_UNorm;
                pixFormat = QOpenGLTexture::RGB;
                pixType   = QOpenGLTexture::UInt8;
            } else if (isARGB32Premul) {
                texFormat = QOpenGLTexture::RGBA8_UNorm;
                pixFormat = QOpenGLTexture::BGRA;
                pixType   = QOpenGLTexture::UInt8;
            } else {
                texFormat = QOpenGLTexture::RGBA8_UNorm;
                pixFormat = QOpenGLTexture::RGBA;
                pixType   = QOpenGLTexture::UInt8;
            }
            m_texture->setFormat(texFormat);
            m_texture->setMinificationFilter(QOpenGLTexture::Linear);
            m_texture->setMagnificationFilter(QOpenGLTexture::Linear);
            m_texture->setWrapMode(QOpenGLTexture::ClampToEdge);
            m_texture->allocateStorage(pixFormat, pixType);
            m_textureFormat = frameFmt;
        }

        // QImage::Format_RGB888 has 3-byte pixels but bytesPerLine is rounded
        // up to a 4-byte boundary. Setting GL_UNPACK_ALIGNMENT=4 is fine for
        // both RGB888 (with stride padding) and RGBA8888 — Qt allocates
        // bytesPerLine to 4-byte alignment, so the default GL value works.
        const QOpenGLTexture::PixelFormat upPixFormat = is16
            ? QOpenGLTexture::RGBA
            : (isRGB888 ? QOpenGLTexture::RGB
                : (isARGB32Premul ? QOpenGLTexture::BGRA
                    : QOpenGLTexture::RGBA));
        const QOpenGLTexture::PixelType upPixType = is16
            ? QOpenGLTexture::UInt16
            : QOpenGLTexture::UInt8;
        m_texture->setData(0, 0, upPixFormat, upPixType,
                           static_cast<const void*>(uploadFrame.constBits()));
        m_needsUpload = false;
    }

    if (!m_texture || !m_program) return;

    m_program->bind();
    m_texture->bind();
    glViewport(viewportX, viewportY, viewportW, viewportH);

    // Set uniforms
    m_program->setUniformValue(m_locTexture, 0);
    m_program->setUniformValue(m_locEffectsEnabled, m_effectsEnabled);
    if (m_locClipOpacity != -1)
        m_program->setUniformValue(m_locClipOpacity, static_cast<float>(clipOpacity));
    m_program->setUniformValue(m_locHdrTransfer, m_hdrTransfer);

    // Seed every Fx uniform to off/zero; enable only those found in m_videoEffects.
    bool  fxBlur = false, fxNoise = false, fxSepia = false;
    bool  fxGray = false, fxInvert = false, fxVignette = false, fxSharpen = false;
    bool  fxMosaic = false, fxChromaKey = false;
    float fxBlurR = 0.0f, fxNoiseA = 0.0f, fxSepiaS = 0.0f;
    float fxGrayS = 0.0f, fxInvertS = 0.0f;
    float fxVigI = 0.0f, fxVigR = 0.75f, fxSharpenA = 0.0f;
    float fxMosaicSize = 0.0f, fxChromaTolerance = 0.0f;
    QVector3D fxChromaKeyColor(0.0f, 1.0f, 0.0f);
    for (const VideoEffect &e : m_videoEffects) {
        if (!e.enabled) continue;
        switch (e.type) {
        case VideoEffectType::Blur:
            fxBlur = true;
            fxBlurR = static_cast<float>(qBound(0.0, e.param1, 8.0));
            break;
        case VideoEffectType::Noise:
            fxNoise = true;
            fxNoiseA = static_cast<float>(qBound(0.0, e.param1, 100.0) / 100.0 * 0.4);
            break;
        case VideoEffectType::Sepia:
            fxSepia = true;
            fxSepiaS = static_cast<float>(qBound(0.0, e.param1, 1.0));
            break;
        case VideoEffectType::Grayscale:
            fxGray = true;
            fxGrayS = static_cast<float>(e.param1 > 0.0 ? qBound(0.0, e.param1, 1.0) : 1.0);
            break;
        case VideoEffectType::Invert:
            fxInvert = true;
            fxInvertS = static_cast<float>(e.param1 > 0.0 ? qBound(0.0, e.param1, 1.0) : 1.0);
            break;
        case VideoEffectType::Vignette:
            fxVignette = true;
            fxVigI = static_cast<float>(qBound(0.0, e.param1, 1.0));
            fxVigR = static_cast<float>(qBound(0.0, e.param2, 1.0));
            break;
        case VideoEffectType::Sharpen:
            fxSharpen = true;
            fxSharpenA = static_cast<float>(qBound(0.0, e.param1, 2.0));
            break;
        case VideoEffectType::Mosaic:
            fxMosaic = true;
            fxMosaicSize = static_cast<float>(qBound(0.0, e.param1, 100.0));
            break;
        case VideoEffectType::ChromaKey:
            fxChromaKey = true;
            fxChromaKeyColor = QVector3D(static_cast<float>(e.keyColor.redF()),
                                         static_cast<float>(e.keyColor.greenF()),
                                         static_cast<float>(e.keyColor.blueF()));
            fxChromaTolerance = static_cast<float>(qBound(0.0, e.param1, 255.0) / 255.0);
            break;
        default:
            break;
        }
    }
    m_program->setUniformValue(m_locFxBlurEnable, fxBlur);
    m_program->setUniformValue(m_locFxBlurRadius, fxBlurR);
    m_program->setUniformValue(m_locFxTexSize,
        QVector2D(m_texture ? static_cast<float>(m_texture->width())  : 1.0f,
                  m_texture ? static_cast<float>(m_texture->height()) : 1.0f));
    m_program->setUniformValue(m_locFxNoiseEnable, fxNoise);
    m_program->setUniformValue(m_locFxNoiseAmount, fxNoiseA);
    m_program->setUniformValue(m_locFxSepiaEnable, fxSepia);
    m_program->setUniformValue(m_locFxSepiaStrength, fxSepiaS);
    m_program->setUniformValue(m_locFxGrayEnable, fxGray);
    m_program->setUniformValue(m_locFxGrayStrength, fxGrayS);
    m_program->setUniformValue(m_locFxInvertEnable, fxInvert);
    m_program->setUniformValue(m_locFxInvertStrength, fxInvertS);
    m_program->setUniformValue(m_locFxVignetteEnable, fxVignette);
    m_program->setUniformValue(m_locFxVignetteIntensity, fxVigI);
    m_program->setUniformValue(m_locFxVignetteRadius, fxVigR);
    m_program->setUniformValue(m_locFxSharpenEnable, fxSharpen);
    m_program->setUniformValue(m_locFxSharpenAmount, fxSharpenA);
    m_program->setUniformValue(m_locFxMosaicEnable, fxMosaic);
    m_program->setUniformValue(m_locFxMosaicSize, fxMosaicSize);
    m_program->setUniformValue(m_locFxChromaKeyEnable, fxChromaKey);
    m_program->setUniformValue(m_locFxChromaKey, fxChromaKeyColor);
    m_program->setUniformValue(m_locFxChromaTolerance, fxChromaTolerance);
    m_program->setUniformValue(m_locFxTime,
        static_cast<float>(QDateTime::currentMSecsSinceEpoch() % 1000000) / 1000.0f);
    m_program->setUniformValue(m_locBrightness,  static_cast<float>(m_cc.brightness));
    m_program->setUniformValue(m_locContrast,    static_cast<float>(m_cc.contrast));
    m_program->setUniformValue(m_locSaturation,  static_cast<float>(m_cc.saturation));
    m_program->setUniformValue(m_locHue,         static_cast<float>(m_cc.hue));
    m_program->setUniformValue(m_locTemperature, static_cast<float>(m_cc.temperature));
    m_program->setUniformValue(m_locTint,        static_cast<float>(m_cc.tint));
    m_program->setUniformValue(m_locGamma,       static_cast<float>(m_cc.gamma));
    m_program->setUniformValue(m_locHighlights,  static_cast<float>(m_cc.highlights));
    m_program->setUniformValue(m_locShadows,     static_cast<float>(m_cc.shadows));
    m_program->setUniformValue(m_locExposure,    static_cast<float>(m_cc.exposure));

    // US-INT-1: merge any adjustment-layers covering the current timeline
    // position on top of the per-clip cached uniforms. The cached values
    // (m_wb / m_vig* / m_liftGammaGain) are already in shader-uniform space
    // (sliders → gains/exponents) so we transform composite slider values
    // identically to ColorGradingPanel before merging. Empty layer list →
    // composite.gradingEnabled=false → effective_* = cached_* exactly.
    float effWbR = m_wb[0], effWbG = m_wb[1], effWbB = m_wb[2];
    float effVigAmount = m_vigAmount, effVigMid = m_vigMid;
    float effVigRound = m_vigRound, effVigFeather = m_vigFeather;
    auto effLgg = m_liftGammaGain;
    if (m_timeline) {
        qint64 tlUs = 0;
        if (auto *vp = qobject_cast<VideoPlayer *>(parentWidget()))
            tlUs = vp->timelinePositionUs();
        const AdjustmentLayerComposite comp =
            composeAdjustmentLayersAt(m_timeline->adjustmentLayers(), tlUs);
        if (comp.gradingEnabled) {
            // White Balance: transform composite sliders → gains using the
            // same formula ColorGradingPanel::onWhiteBalanceChanged uses.
            const double K     = 5500.0 + comp.wbTempSlider * 30.0;
            double rGain       = std::clamp(1.0 + (5500.0 - K) / 3000.0, 0.5, 2.0);
            double bGain       = std::clamp(1.0 + (K - 5500.0) / 3000.0, 0.5, 2.0);
            const double gGain = 1.0 - (comp.wbTintSlider / 100.0) * 0.4;
            // Multiplicative on top of per-clip WB.
            effWbR *= static_cast<float>(rGain);
            effWbG *= static_cast<float>(gGain);
            effWbB *= static_cast<float>(bGain);

            // Lift / Gamma / Gain: transform composite sliders → uniform
            // space identically to GLPreview::setLiftGammaGain.
            for (int ch = 0; ch < 4; ++ch) {
                const double liftU  = comp.lift[ch]  * 0.5;
                const double gammaU = comp.gamma[ch];           // identity = 0 here
                const double gainU  = std::pow(2.0, comp.gain[ch] * 2.0);
                // lift additive, gamma + gain multiplicative. Composite
                // gamma defaults to 0 (untouched); only multiply when the
                // composite explicitly overrode gamma.
                effLgg[0][ch] += liftU;
                if (gammaU != 0.0)
                    effLgg[1][ch] *= gammaU;
                effLgg[2][ch] *= gainU;
            }

            // Vignette: amount = max(per-clip, composite). When composite
            // wins we also adopt its midpoint/roundness/feather.
            const float compAmount = static_cast<float>(comp.vigAmount);
            if (std::fabs(compAmount) > std::fabs(effVigAmount)) {
                effVigAmount  = compAmount;
                effVigMid     = static_cast<float>(comp.vigMidpoint);
                effVigRound   = static_cast<float>(comp.vigRoundness);
                effVigFeather = static_cast<float>(comp.vigFeather);
            }
        }
    }

    // US-CG-2: White-balance gain triple — uploaded every draw because the
    // shader test `uWb != vec3(1.0)` skips the multiply at identity.
    m_program->setUniformValue(m_locWb,
        QVector3D(effWbR, effWbG, effWbB));

    // US-CG-3: Radial vignette — uploaded every draw. The shader test
    // `abs(uVigAmount) > 0.0001` keeps amount==0 a free no-op.
    m_program->setUniformValue(m_locVigAmount,  effVigAmount);
    m_program->setUniformValue(m_locVigMid,     effVigMid);
    m_program->setUniformValue(m_locVigRound,   effVigRound);
    m_program->setUniformValue(m_locVigFeather, effVigFeather);

    // US-EF-1: Chroma Key — uploaded every draw. uChromaEnabled=false is a
    // free no-op (entire stage skipped in the fragment shader).
    m_program->setUniformValue(m_locChromaEnabled, m_chromaEnabled);
    m_program->setUniformValue(m_locChromaKeyHsl,
        QVector3D(m_chromaKeyH, m_chromaKeyS, m_chromaKeyL));
    m_program->setUniformValue(m_locChromaTol,
        QVector3D(m_chromaHueTol, m_chromaSatTol, m_chromaLumTol));
    m_program->setUniformValue(m_locChromaSpill, m_chromaSpillStrength);
    m_program->setUniformValue(m_locChromaSoft,  m_chromaSoftness);

    // US-EF-3: HSL Qualifier — uploaded every draw. uHslqEnabled=false is a
    // free no-op (entire stage skipped in the fragment shader).
    m_program->setUniformValue(m_locHslqEnabled,   m_hslqEnabled);
    m_program->setUniformValue(m_locHslqHueCenter, m_hslqHueCenter);
    m_program->setUniformValue(m_locHslqHueRange,  m_hslqHueRange);
    m_program->setUniformValue(m_locHslqSatRange,
        QVector2D(m_hslqSatMin, m_hslqSatMax));
    m_program->setUniformValue(m_locHslqLumaRange,
        QVector2D(m_hslqLumaMin, m_hslqLumaMax));
    m_program->setUniformValue(m_locHslqSoftness,  m_hslqSoftness);
    m_program->setUniformValue(m_locHslqLift,
        QVector3D(m_hslqLift[0], m_hslqLift[1], m_hslqLift[2]));
    m_program->setUniformValue(m_locHslqGamma,
        QVector3D(m_hslqGamma[0], m_hslqGamma[1], m_hslqGamma[2]));
    m_program->setUniformValue(m_locHslqGain,
        QVector3D(m_hslqGain[0], m_hslqGain[1], m_hslqGain[2]));

    // US-EF-2: Mask Animation — uploaded every draw. uMaskEnabled=false is
    // a free no-op (the wrap mix() is skipped in the fragment shader).
    m_program->setUniformValue(m_locMaskEnabled, m_maskEnabled);
    m_program->setUniformValue(m_locMaskEllipse, m_maskEllipse);
    m_program->setUniformValue(m_locMaskInvert,  m_maskInvert);
    m_program->setUniformValue(m_locMaskFeather, m_maskFeather);
    m_program->setUniformValue(m_locMaskRect,
        QVector4D(m_maskRect[0], m_maskRect[1], m_maskRect[2], m_maskRect[3]));

    // US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens Distortion.
    // Identity at 0 keeps each stage a free no-op (the shader's |amount|>eps
    // tests skip the kernel/transform entirely).
    m_program->setUniformValue(m_locSharpenAmount,  m_sharpenAmount);
    m_program->setUniformValue(m_locBlurRadius,     m_blurRadius);
    m_program->setUniformValue(m_locLensDistortion, m_lensDistortion);

    // US-3D: 3-axis rotation + perspective. Ship the CPU-built 3x3 rotation
    // matrix as a QMatrix3x3 (column-major float[9]). Identity is a free
    // no-op — the shader skips the entire warp when uRot3DEnabled=false.
    m_program->setUniformValue(m_locRot3DEnabled, m_rot3DEnabled);
    if (m_rot3DEnabled && m_locRot3D != -1) {
        QMatrix3x3 mat(m_rot3DMatrix);
        m_program->setUniformValue(m_locRot3D, mat);
    }
    m_program->setUniformValue(m_locPerspectiveDist, m_perspectiveDist);

    // US-INT-4: stabilizer pre-warp matrix. Identity = free no-op.
    if (m_locStabEnabled != -1)
        m_program->setUniformValue(m_locStabEnabled, m_stabEnabled);
    if (m_stabEnabled && m_locStab != -1) {
        QMatrix3x3 stabMat(m_stabMatrix);
        m_program->setUniformValue(m_locStab, stabMat);
    }

    // Lift/Gamma/Gain (US-WIRE-2: vec4: xyz=RGB, w=Luma)
    m_program->setUniformValue(m_locLift,
        QVector4D(static_cast<float>(effLgg[0][0]),
                  static_cast<float>(effLgg[0][1]),
                  static_cast<float>(effLgg[0][2]),
                  static_cast<float>(effLgg[0][3])));
    m_program->setUniformValue(m_locLggGamma,
        QVector4D(static_cast<float>(effLgg[1][0]),
                  static_cast<float>(effLgg[1][1]),
                  static_cast<float>(effLgg[1][2]),
                  static_cast<float>(effLgg[1][3])));
    m_program->setUniformValue(m_locGain,
        QVector4D(static_cast<float>(effLgg[2][0]),
                  static_cast<float>(effLgg[2][1]),
                  static_cast<float>(effLgg[2][2]),
                  static_cast<float>(effLgg[2][3])));

    // LUT
    m_program->setUniformValue(m_locLutEnabled, m_lutEnabled);
    m_program->setUniformValue(m_locLutIntensity, m_lutIntensity);
    if (m_lutEnabled && m_lutTexture) {
        glActiveTexture(GL_TEXTURE1);
        m_lutTexture->bind();
        m_program->setUniformValue(m_locLut3D, 1);
    }

    // US-CG-1: RGB Curves LUT — bind on texture unit 2 when enabled.
    // Pending uploads (queued via setRgbCurves before initializeGL) are
    // flushed here because the GL context is guaranteed current.
    if (m_curvesNeedUpload && !m_pendingCurves.isEmpty()) {
        QVector<unsigned char> data(256 * 4 * 4, 0);
        for (int row = 0; row < 4 && row < m_pendingCurves.size(); ++row) {
            const QVector<int> &curve = m_pendingCurves[row];
            for (int x = 0; x < 256; ++x) {
                int v = (x < curve.size()) ? curve[x] : x;
                v = std::clamp(v, 0, 255);
                int idx = (row * 256 + x) * 4;
                data[idx + 0] = static_cast<unsigned char>(v);
                data[idx + 1] = static_cast<unsigned char>(v);
                data[idx + 2] = static_cast<unsigned char>(v);
                data[idx + 3] = static_cast<unsigned char>(v);
            }
        }
        if (!m_curveLutTex) {
            m_curveLutTex = new QOpenGLTexture(QOpenGLTexture::Target2D);
            m_curveLutTex->setSize(256, 4);
            m_curveLutTex->setFormat(QOpenGLTexture::RGBA8_UNorm);
            m_curveLutTex->allocateStorage();
            m_curveLutTex->setMinificationFilter(QOpenGLTexture::Linear);
            m_curveLutTex->setMagnificationFilter(QOpenGLTexture::Linear);
            m_curveLutTex->setWrapMode(QOpenGLTexture::ClampToEdge);
        }
        m_curveLutTex->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                               data.constData());
        m_curvesNeedUpload = false;
    }
    m_program->setUniformValue(m_locCurvesEnabled, m_curvesEnabled);
    if (m_curvesEnabled && m_curveLutTex) {
        glActiveTexture(GL_TEXTURE2);
        m_curveLutTex->bind();
        m_program->setUniformValue(m_locCurveLut, 2);
    }

    // US-CG-4: Hue vs Saturation curve LUT — bind on texture unit 3 when
    // enabled. Pending uploads (queued via setHueVsSatLut before the GL
    // context was current) are flushed here.
    if (m_hueVsSatNeedUpload && m_pendingHueVsSatLut.size() == 256) {
        QVector<unsigned char> data(256, 0);
        for (int x = 0; x < 256; ++x) {
            // Slider range is 0..2.0; pack into a single byte 0..255 by
            // halving so 1.0 (identity) lands exactly on 128. The shader
            // un-packs by multiplying the sample by 2.0.
            float v = m_pendingHueVsSatLut[x] * 0.5f;
            v = std::clamp(v, 0.0f, 1.0f);
            data[x] = static_cast<unsigned char>(std::lround(v * 255.0f));
        }
        if (!m_hueVsSatTex) {
            m_hueVsSatTex = new QOpenGLTexture(QOpenGLTexture::Target2D);
            m_hueVsSatTex->setSize(256, 1);
            m_hueVsSatTex->setFormat(QOpenGLTexture::R8_UNorm);
            m_hueVsSatTex->allocateStorage();
            m_hueVsSatTex->setMinificationFilter(QOpenGLTexture::Linear);
            m_hueVsSatTex->setMagnificationFilter(QOpenGLTexture::Linear);
            m_hueVsSatTex->setWrapMode(QOpenGLTexture::Repeat);
        }
        m_hueVsSatTex->setData(QOpenGLTexture::Red, QOpenGLTexture::UInt8,
                               data.constData());
        m_hueVsSatNeedUpload = false;
    }
    m_program->setUniformValue(m_locHueVsSatEnabled, m_hueVsSatEnabled);
    if (m_hueVsSatEnabled && m_hueVsSatTex) {
        glActiveTexture(GL_TEXTURE3);
        m_hueVsSatTex->bind();
        m_program->setUniformValue(m_locHueVsSatLut, 3);
    }

    // Draw quad
    m_vao.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();

    if (m_curvesEnabled && m_curveLutTex) {
        glActiveTexture(GL_TEXTURE2);
        m_curveLutTex->release();
        glActiveTexture(GL_TEXTURE0);
    }
    if (m_hueVsSatEnabled && m_hueVsSatTex) {
        glActiveTexture(GL_TEXTURE3);
        m_hueVsSatTex->release();
        glActiveTexture(GL_TEXTURE0);
    }
    if (m_lutEnabled && m_lutTexture) {
        glActiveTexture(GL_TEXTURE1);
        m_lutTexture->release();
        glActiveTexture(GL_TEXTURE0);
    }

    m_texture->release();
    m_program->release();
    glViewport(0, 0, physW, physH);

    // Adobe-style text tool overlay: draw the dashed marquee plus 8 resize
    // handles while the tool is active and a rect is present (either being
    // created/moved/resized this frame, or persisted from a previous drag).
    if (m_textToolActive && (m_textToolMode != TextToolIdle || m_textToolHasRect)) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // Match VideoPlayer::composeFrameWithOverlays so inline text edges
        // align with the committed overlay at the same font size.
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        QPen pen(QColor(255, 255, 255, 230));
        pen.setStyle(Qt::DashLine);
        pen.setWidth(2);
        painter.setPen(pen);
        painter.setBrush(QColor(255, 255, 255, 30));
        const QRect r = (m_textToolMode == TextToolCreating)
            ? QRect(m_textToolPressPos, m_textToolCurrentPos).normalized()
            : m_textToolRect;
        painter.drawRect(r);

        // Handles — only when we have a persisted rect (not while creating).
        if (m_textToolHasRect && m_textToolMode != TextToolCreating) {
            painter.setPen(QPen(QColor(255, 255, 255, 240), 1));
            painter.setBrush(QColor(40, 120, 220, 230));
            const int hs = 7; // handle size
            const QPoint corners[8] = {
                r.topLeft(),                                  // TL
                QPoint(r.center().x(), r.top()),              // T
                r.topRight(),                                 // TR
                QPoint(r.left(),  r.center().y()),            // L
                QPoint(r.right(), r.center().y()),            // R
                r.bottomLeft(),                               // BL
                QPoint(r.center().x(), r.bottom()),           // B
                r.bottomRight()                               // BR
            };
            for (const auto &c : corners)
                painter.drawRect(QRect(c.x() - hs / 2, c.y() - hs / 2, hs, hs));
        }

        // In-place text rendering (Adobe-style live edit): draw the text
        // being typed inside the rect, plus a blinking caret at the end of
        // the text. Only drawn in edit mode — the final commit goes through
        // MainWindow::applyTextToolOverlay via textInlineCommitted.
        if (m_textToolEditing && m_textToolHasRect) {
            // US-T32 WYSIWYG: draw at the literal configured pointSize in
            // widget coordinates. The committed path in
            // VideoPlayer::composeFrameWithOverlays inverse-scales by
            // imageH / letterboxH so both render at the same visible pt.
            QFont f = m_textToolStyleFont;
            if (f.pointSizeF() <= 0.0) f.setPointSizeF(32.0);
            painter.setFont(f);
            painter.setPen(m_textToolStyleColor);
            const int alignFlags = Qt::AlignCenter | Qt::TextWordWrap;
            painter.drawText(r, alignFlags, m_textToolInputText);
            if (m_textToolCaretVisible) {
                // Caret tracks the end of the last drawn glyph so it stays
                // attached to the centered text regardless of width.
                const QFontMetrics fm(f);
                const QString probe = m_textToolInputText.isEmpty()
                    ? QStringLiteral(" ")
                    : m_textToolInputText;
                const QRect textBounds = fm.boundingRect(r, alignFlags, probe);
                const int caretX = m_textToolInputText.isEmpty()
                    ? textBounds.left()
                    : textBounds.right() + 2;
                QPen caretPen(m_textToolStyleColor);
                caretPen.setWidth(2);
                painter.setPen(caretPen);
                painter.drawLine(caretX,
                                 qMax(r.top(),    textBounds.top()),
                                 caretX,
                                 qMin(r.bottom(), textBounds.bottom()));
            }
        }
    }

    // US-T36 Permanent 16:9 export canvas frame — drawn on every repaint as
    // a static visual reference so users understand what portion of their
    // composition ends up inside the exported video. Drawn BEFORE the video
    // transform selection so the selection handles render on top.
    {
        const QRectF canvasRect = canvasFrameRect();
        QPainter cpainter(this);
        cpainter.setRenderHint(QPainter::Antialiasing, true);
        QPen canvasPen(QColor(255, 200, 0, 220));
        canvasPen.setStyle(Qt::SolidLine);
        canvasPen.setWidth(2);
        cpainter.setPen(canvasPen);
        cpainter.setBrush(Qt::NoBrush);
        cpainter.drawRect(canvasRect);
    }

    // US-T34 OBS-style video transform selection overlay — drawn only when
    // the text tool is off and the user has selected the video source.
    if (!m_textToolActive && m_videoTransformSelected) {
        const QRectF vr = videoDisplayRect();
        if (vr.width() > 2.0 && vr.height() > 2.0) {
            QPainter vpainter(this);
            vpainter.setRenderHint(QPainter::Antialiasing, true);
            QPen outline(QColor(80, 200, 255, 230));
            outline.setWidth(2);
            vpainter.setPen(outline);
            vpainter.setBrush(Qt::NoBrush);
            vpainter.drawRect(vr);
            // 8 handles (4 corners + 4 edges)
            const double cx = vr.center().x();
            const double cy = vr.center().y();
            const QPointF pts[8] = {
                vr.topLeft(),    {cx, vr.top()},    vr.topRight(),
                {vr.left(), cy},                    {vr.right(), cy},
                vr.bottomLeft(), {cx, vr.bottom()}, vr.bottomRight()
            };
            const int size = 10;
            vpainter.setBrush(QColor(80, 200, 255, 230));
            QPen handlePen(QColor(255, 255, 255, 230));
            handlePen.setWidth(1);
            vpainter.setPen(handlePen);
            for (const QPointF &p : pts) {
                vpainter.drawRect(QRectF(p.x() - size / 2.0, p.y() - size / 2.0, size, size));
            }
        }
    }

    // US-MOCHA-3: SurfaceTool overlay — 4-corner pin gizmo drawn after all
    // other overlay visuals so it renders on top.
    if (m_surfaceTool && m_surfaceTool->isEnabled()) {
        QPainter spainter(this);
        spainter.setRenderHint(QPainter::Antialiasing, true);
        m_surfaceTool->paintOverlay(spainter, letterboxRect());
    }
}

QRectF GLPreview::letterboxRect() const
{
    const double W = qMax(1, width());
    const double H = qMax(1, height());
    if (m_displayAspectRatio <= 0.0 || !std::isfinite(m_displayAspectRatio))
        return QRectF(0, 0, W, H);
    const double widgetAspect = W / H;
    if (widgetAspect > m_displayAspectRatio) {
        // vertical bars on the sides
        const double w = H * m_displayAspectRatio;
        return QRectF((W - w) / 2.0, 0.0, w, H);
    }
    // horizontal bars on top/bottom
    const double h = W / m_displayAspectRatio;
    return QRectF(0.0, (H - h) / 2.0, W, h);
}

QRectF GLPreview::canvasFrameRect() const
{
    const double W = qMax(1, width());
    const double H = qMax(1, height());
    const double canvasAspect = 16.0 / 9.0;
    double cw = W;
    double ch = W / canvasAspect;
    if (ch > H) {
        ch = H;
        cw = H * canvasAspect;
    }
    return QRectF((W - cw) / 2.0, (H - ch) / 2.0, cw, ch);
}

void GLPreview::setVideoSourceTransform(double scale, double dx, double dy)
{
    m_videoSourceScale = qBound(0.1, scale, 10.0);
    m_videoSourceDx = qBound(-5.0, dx, 5.0);
    m_videoSourceDy = qBound(-5.0, dy, 5.0);
    update();
}

void GLPreview::resetVideoSourceTransform()
{
    m_videoSourceScale = 1.0;
    m_videoSourceDx = 0.0;
    m_videoSourceDy = 0.0;
    m_videoTransformSelected = false;
    m_videoDragMode = VideoDragNone;
    m_videoDragHandle = HandleNone;
    emit videoSourceTransformChanged(m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
    update();
}

void GLPreview::setVideoTransformSelected(bool selected)
{
    if (m_videoTransformSelected == selected)
        return;
    m_videoTransformSelected = selected;
    update();
}

void GLPreview::setCompositeBakedMode(bool enabled)
{
    if (m_compositeBakedMode == enabled) return;
    m_compositeBakedMode = enabled;
    update();
}

QRectF GLPreview::videoDisplayRectFor(double scale, double dx, double dy) const
{
    const QRectF lb = letterboxRect();
    const double w = lb.width() * scale;
    const double h = lb.height() * scale;
    const double cx = lb.x() + lb.width() / 2.0 + dx * lb.width();
    const double cy = lb.y() + lb.height() / 2.0 + dy * lb.height();
    return QRectF(cx - w / 2.0, cy - h / 2.0, w, h);
}

QRectF GLPreview::videoDisplayRect() const
{
    return videoDisplayRectFor(m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
}

GLPreview::TextToolHandle GLPreview::hitTestVideoHandle(const QPoint &pt) const
{
    const QRectF r = videoDisplayRect();
    if (r.isEmpty())
        return HandleNone;
    const int size = 14;
    auto hitBox = [&](double cx, double cy) {
        return QRectF(cx - size / 2.0, cy - size / 2.0, size, size).contains(pt);
    };
    const double cx = r.center().x();
    const double cy = r.center().y();
    const TextToolHandle ids[8] = {
        HandleTL, HandleT, HandleTR,
        HandleL,         HandleR,
        HandleBL, HandleB, HandleBR
    };
    const QPointF pts[8] = {
        r.topLeft(),     {cx, r.top()},    r.topRight(),
        {r.left(), cy},                    {r.right(), cy},
        r.bottomLeft(),  {cx, r.bottom()}, r.bottomRight()
    };
    for (int i = 0; i < 8; ++i) {
        if (hitBox(pts[i].x(), pts[i].y())) return ids[i];
    }
    return HandleNone;
}

bool GLPreview::pointInsideVideoRect(const QPoint &pt) const
{
    return videoDisplayRect().contains(pt);
}

void GLPreview::enterTextToolEditMode()
{
    m_textToolEditing = true;
    m_textToolInputText.clear();
    m_textToolCaretVisible = true;
    m_textToolCaretTimer.start();
    setFocus(Qt::OtherFocusReason);
    // Editing an existing overlay: tell compose to hide its committed
    // render so inline edit doesn't overlap. Fresh create-drag has index -1.
    if (m_textToolEditingIndex >= 0)
        emit textOverlayEditStarted(m_textToolEditingIndex);
    update();
}

void GLPreview::exitTextToolEditMode()
{
    const bool wasEditingExisting = (m_textToolEditingIndex >= 0);
    m_textToolEditing = false;
    m_textToolInputText.clear();
    m_textToolEditingIndex = -1;
    m_textToolCaretTimer.stop();
    if (wasEditingExisting)
        emit textOverlayEditEnded();
    update();
}

void GLPreview::setTextToolStyle(const QFont &font, const QColor &color)
{
    m_textToolStyleFont = font;
    m_textToolStyleColor = color;
    if (m_textToolEditing)
        update();
}

void GLPreview::setTextOverlayHitList(const QVector<TextOverlayHit> &hits)
{
    m_textToolOverlayHits = hits;
}

void GLPreview::commitCurrentTextToolEdit()
{
    if (!m_textToolEditing)
        return;
    // Capture state THEN exit edit mode BEFORE emitting. MainWindow's
    // applyTextToolOverlay slot re-checks isTextToolEditing() and calls
    // back into this function when true, so emitting first would recurse
    // through the signal chain until the stack overflows (reproduced by
    // pressing Enter on the preview text tool — the app crashes).
    const QString text = m_textToolInputText;
    const int editingIndex = m_textToolEditingIndex;
    const QRectF lb = letterboxRect();
    const double lbw = qMax(1.0, lb.width());
    const double lbh = qMax(1.0, lb.height());
    const QRectF normalized((m_textToolRect.x() - lb.x()) / lbw,
                            (m_textToolRect.y() - lb.y()) / lbh,
                            m_textToolRect.width()  / lbw,
                            m_textToolRect.height() / lbh);
    exitTextToolEditMode();
    if (text.isEmpty())
        return;
    if (editingIndex >= 0) {
        emit textOverlayEditCommitted(editingIndex, text);
    } else {
        emit textInlineCommitted(text, normalized);
    }
}

int GLPreview::hitTestExistingOverlay(const QPoint &widgetPos) const
{
    if (m_textToolOverlayHits.isEmpty())
        return -1;
    const QRectF lb = letterboxRect();
    // EnhancedTextOverlay stores IMAGE-normalized center + size; convert
    // to widget pixels via letterboxRect so the hit area lines up with the
    // actually-rendered text, not with the full widget.
    for (const auto &hit : m_textToolOverlayHits) {
        if (hit.normalizedRect.width() <= 0.0 || hit.normalizedRect.height() <= 0.0)
            continue;
        const double cx = hit.normalizedRect.x();
        const double cy = hit.normalizedRect.y();
        const double bw = hit.normalizedRect.width();
        const double bh = hit.normalizedRect.height();
        QRect r(static_cast<int>(lb.x() + (cx - bw / 2.0) * lb.width()),
                static_cast<int>(lb.y() + (cy - bh / 2.0) * lb.height()),
                static_cast<int>(bw * lb.width()),
                static_cast<int>(bh * lb.height()));
        if (r.contains(widgetPos))
            return hit.index;
    }
    return -1;
}

GLPreview::TextToolHandle GLPreview::hitTestTextToolHandle(const QPoint &pt) const
{
    if (!m_textToolHasRect)
        return HandleNone;
    const QRect &r = m_textToolRect;
    const int tol = 8;
    auto near = [tol](const QPoint &a, const QPoint &b) {
        return qAbs(a.x() - b.x()) <= tol && qAbs(a.y() - b.y()) <= tol;
    };
    const QPoint corners[8] = {
        r.topLeft(),
        QPoint(r.center().x(), r.top()),
        r.topRight(),
        QPoint(r.left(),  r.center().y()),
        QPoint(r.right(), r.center().y()),
        r.bottomLeft(),
        QPoint(r.center().x(), r.bottom()),
        r.bottomRight()
    };
    const TextToolHandle ids[8] = {
        HandleTL, HandleT, HandleTR,
        HandleL,  HandleR,
        HandleBL, HandleB, HandleBR
    };
    for (int i = 0; i < 8; ++i)
        if (near(pt, corners[i])) return ids[i];
    return HandleNone;
}

bool GLPreview::pointInsideTextToolRect(const QPoint &pt) const
{
    return m_textToolHasRect && m_textToolRect.contains(pt);
}

void GLPreview::emitCurrentTextToolRect()
{
    // Note: an 8x8 minimum is enforced in mouseReleaseEvent for the CREATE
    // path so accidental clicks don't generate ghost boxes. Move / resize
    // releases always re-emit (even if the user collapsed the rect) so
    // MainWindow's pending rect stays in sync with the visible geometry.
    const QRectF lb = letterboxRect();
    const double lbw = qMax(1.0, lb.width());
    const double lbh = qMax(1.0, lb.height());
    QRectF normalized((m_textToolRect.x() - lb.x()) / lbw,
                      (m_textToolRect.y() - lb.y()) / lbh,
                      m_textToolRect.width()  / lbw,
                      m_textToolRect.height() / lbh);
    emit textRectRequested(normalized);
}

void GLPreview::setTextToolActive(bool active)
{
    if (m_textToolActive == active)
        return;
    m_textToolActive = active;
    // Hover-cursor feedback in idle mode requires tracking mouse moves
    // without a pressed button. Focus policy allows keyPressEvent to fire
    // for in-place text editing after a drag-create.
    setMouseTracking(active);
    setFocusPolicy(active ? Qt::StrongFocus : Qt::NoFocus);
    if (active) {
        setCursor(Qt::IBeamCursor);
    } else {
        unsetCursor();
        m_textToolMode = TextToolIdle;
        m_textToolHasRect = false;
        exitTextToolEditMode();
    }
    update();
}

void GLPreview::clearTextToolRect()
{
    m_textToolHasRect = false;
    m_textToolMode = TextToolIdle;
    exitTextToolEditMode();
    update();
}

void GLPreview::mousePressEvent(QMouseEvent *event)
{
    // US-MOCHA-3: SurfaceTool intercepts mouse events when installed and enabled.
    if (m_surfaceTool && m_surfaceTool->isEnabled()) {
        if (m_surfaceTool->handleMousePress(event->pos(), event->button(), event->modifiers())) {
            event->accept();
            return;
        }
    }
    // US-T34 Video source transform: only active when the text tool is OFF
    // so it doesn't clash with text creation / click-to-edit gestures.
    if (!m_textToolActive && event->button() == Qt::LeftButton) {
        const TextToolHandle hit = hitTestVideoHandle(event->pos());
        if (hit != HandleNone) {
            m_videoTransformSelected = true;
            m_videoDragMode = VideoDragResizing;
            m_videoDragHandle = hit;
            m_videoDragPressPos = event->pos();
            m_videoDragStartScale = m_videoSourceScale;
            m_videoDragStartDx = m_videoSourceDx;
            m_videoDragStartDy = m_videoSourceDy;
            update();
            event->accept();
            return;
        }
        if (pointInsideVideoRect(event->pos())) {
            m_videoTransformSelected = true;
            m_videoDragMode = VideoDragMoving;
            m_videoDragHandle = HandleNone;
            m_videoDragPressPos = event->pos();
            m_videoDragStartScale = m_videoSourceScale;
            m_videoDragStartDx = m_videoSourceDx;
            m_videoDragStartDy = m_videoSourceDy;
            update();
            event->accept();
            return;
        }
        if (m_videoTransformSelected) {
            m_videoTransformSelected = false;
            update();
        }
    }
    if (!m_textToolActive || event->button() != Qt::LeftButton) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }
    m_textToolPressPos = event->pos();
    m_textToolCurrentPos = event->pos();

    // Probe handle/body BEFORE committing so dragging a handle while editing
    // reshapes in place (Premiere-style). Only an outside click commits.
    const TextToolHandle hit = hitTestTextToolHandle(event->pos());
    const bool insideRect = pointInsideTextToolRect(event->pos());
    if (m_textToolEditing && hit == HandleNone && !insideRect) {
        commitCurrentTextToolEdit();
    } else if (!m_textToolEditing && m_textToolHasRect
               && hit == HandleNone && !insideRect
               && hitTestExistingOverlay(event->pos()) < 0) {
        // OBS-style deselect: outside click with a selection but no active
        // edit and no new overlay under cursor → clear the selection.
        m_textToolHasRect = false;
        m_textToolEditingIndex = -1;
        m_textToolMode = TextToolIdle;
        update();
    }

    if (hit != HandleNone) {
        m_textToolMode = TextToolResizing;
        m_textToolActiveHandle = hit;
        m_textToolDragStartRect = m_textToolRect;
    } else if (insideRect) {
        m_textToolMode = TextToolMoving;
        m_textToolActiveHandle = HandleNone;
        m_textToolDragStartRect = m_textToolRect;
    } else {
        // Adobe click-to-edit: if the click lands on an already-rendered
        // overlay, enter edit mode for THAT overlay instead of starting a
        // new create-drag. Enter commits via textOverlayEditCommitted so
        // MainWindow can update the existing overlay in place.
        const int existingIdx = hitTestExistingOverlay(event->pos());
        if (existingIdx >= 0) {
            const TextOverlayHit *hitInfo = nullptr;
            for (const auto &h : m_textToolOverlayHits) {
                if (h.index == existingIdx) { hitInfo = &h; break; }
            }
            if (hitInfo) {
                // OBS-style: single click on an existing overlay immediately
                // enters a drag mode (move or resize depending on whether the
                // press lands on a handle). We do NOT call enterTextToolEditMode
                // here — that's reserved for mouseDoubleClickEvent — so the user
                // gets press-and-drag to reshape without an in-place caret.
                const QRectF lb = letterboxRect();
                const QRectF &nr = hitInfo->normalizedRect;
                const int rw = static_cast<int>(nr.width()  * lb.width());
                const int rh = static_cast<int>(nr.height() * lb.height());
                const int rx = static_cast<int>(lb.x() + (nr.x() - nr.width()  / 2.0) * lb.width());
                const int ry = static_cast<int>(lb.y() + (nr.y() - nr.height() / 2.0) * lb.height());
                m_textToolRect = QRect(rx, ry, rw, rh);
                m_textToolHasRect = true;
                m_textToolEditingIndex = existingIdx;
                // Re-hit with the freshly-set rect so handle press enters
                // Resizing directly instead of always falling into Moving.
                const TextToolHandle freshHit = hitTestTextToolHandle(event->pos());
                if (freshHit != HandleNone) {
                    m_textToolMode = TextToolResizing;
                    m_textToolActiveHandle = freshHit;
                } else {
                    m_textToolMode = TextToolMoving;
                    m_textToolActiveHandle = HandleNone;
                }
                m_textToolDragStartRect = m_textToolRect;
                update();
                event->accept();
                return;
            }
        }
        m_textToolMode = TextToolCreating;
        m_textToolHasRect = false;
        m_textToolActiveHandle = HandleNone;
        m_textToolEditingIndex = -1;
    }
    update();
    event->accept();
}

void GLPreview::mouseMoveEvent(QMouseEvent *event)
{
    // US-MOCHA-3: SurfaceTool handles mouse move when dragging a corner.
    if (m_surfaceTool && m_surfaceTool->isEnabled()) {
        if (m_surfaceTool->handleMouseMove(event->pos(), event->modifiers())) {
            event->accept();
            return;
        }
    }
    // US-T34 video transform drag / hover cursor
    if (!m_textToolActive) {
        if (m_videoDragMode != VideoDragNone) {
            const QPoint delta = event->pos() - m_videoDragPressPos;
            const QRectF lb = letterboxRect();
            const double lbw = qMax(1.0, lb.width());
            const double lbh = qMax(1.0, lb.height());
            if (m_videoDragMode == VideoDragMoving) {
                m_videoSourceDx = m_videoDragStartDx + delta.x() / lbw;
                m_videoSourceDy = m_videoDragStartDy + delta.y() / lbh;
            } else if (m_videoDragMode == VideoDragResizing) {
                // Uniform scale with opposite-corner anchoring. For corner
                // handles the anchor is the opposite corner; for side handles
                // the anchor is the opposite side's midpoint. Scale comes from
                // the mouse's absolute offset from the anchor along whichever
                // axis the handle drives.
                const QRectF startRect = videoDisplayRectFor(
                    m_videoDragStartScale, m_videoDragStartDx, m_videoDragStartDy);
                QPointF anchor = startRect.center();
                bool driveX = false, driveY = false;
                switch (m_videoDragHandle) {
                    case HandleTL: anchor = startRect.bottomRight(); driveX = driveY = true; break;
                    case HandleTR: anchor = startRect.bottomLeft();  driveX = driveY = true; break;
                    case HandleBL: anchor = startRect.topRight();    driveX = driveY = true; break;
                    case HandleBR: anchor = startRect.topLeft();     driveX = driveY = true; break;
                    case HandleT:  anchor = QPointF(startRect.center().x(), startRect.bottom()); driveY = true; break;
                    case HandleB:  anchor = QPointF(startRect.center().x(), startRect.top());    driveY = true; break;
                    case HandleL:  anchor = QPointF(startRect.right(),      startRect.center().y()); driveX = true; break;
                    case HandleR:  anchor = QPointF(startRect.left(),       startRect.center().y()); driveX = true; break;
                    default: break;
                }
                const double mouseDX = qAbs(event->pos().x() - anchor.x());
                const double mouseDY = qAbs(event->pos().y() - anchor.y());
                const double scaleFromX = mouseDX / lbw;
                const double scaleFromY = mouseDY / lbh;
                double newScale = m_videoDragStartScale;
                if (driveX && driveY) newScale = qMax(scaleFromX, scaleFromY);
                else if (driveX)      newScale = scaleFromX;
                else if (driveY)      newScale = scaleFromY;
                newScale = qBound(0.1, newScale, 10.0);
                const double newWpx = lbw * newScale;
                const double newHpx = lbh * newScale;
                QPointF newCenter = anchor;
                switch (m_videoDragHandle) {
                    case HandleTL: newCenter = QPointF(anchor.x() - newWpx / 2.0, anchor.y() - newHpx / 2.0); break;
                    case HandleTR: newCenter = QPointF(anchor.x() + newWpx / 2.0, anchor.y() - newHpx / 2.0); break;
                    case HandleBL: newCenter = QPointF(anchor.x() - newWpx / 2.0, anchor.y() + newHpx / 2.0); break;
                    case HandleBR: newCenter = QPointF(anchor.x() + newWpx / 2.0, anchor.y() + newHpx / 2.0); break;
                    case HandleT:  newCenter = QPointF(startRect.center().x(), anchor.y() - newHpx / 2.0); break;
                    case HandleB:  newCenter = QPointF(startRect.center().x(), anchor.y() + newHpx / 2.0); break;
                    case HandleL:  newCenter = QPointF(anchor.x() - newWpx / 2.0, startRect.center().y()); break;
                    case HandleR:  newCenter = QPointF(anchor.x() + newWpx / 2.0, startRect.center().y()); break;
                    default: break;
                }
                const QPointF lbCenter = lb.center();
                m_videoSourceScale = newScale;
                m_videoSourceDx = qBound(-5.0, (newCenter.x() - lbCenter.x()) / lbw, 5.0);
                m_videoSourceDy = qBound(-5.0, (newCenter.y() - lbCenter.y()) / lbh, 5.0);
            }
            // US-T37 OBS-style snap: if any edge of the transformed video
            // rect lands within a pixel tolerance of the 16:9 canvas frame
            // edge, pull it flush. We first adjust dx/dy so the nearest
            // edge matches; the scale has already been committed above.
            if (m_snapStrength > 0.0) {
                const QRectF canvas = canvasFrameRect();
                const QRectF cur = videoDisplayRectFor(
                    m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
                const double snapPx = m_snapStrength;
                double shiftX = 0.0;
                double shiftY = 0.0;
                if (qAbs(cur.left()   - canvas.left())   < snapPx) shiftX = canvas.left()   - cur.left();
                else if (qAbs(cur.right()  - canvas.right())  < snapPx) shiftX = canvas.right()  - cur.right();
                else if (qAbs(cur.center().x() - canvas.center().x()) < snapPx)
                    shiftX = canvas.center().x() - cur.center().x();
                if (qAbs(cur.top()    - canvas.top())    < snapPx) shiftY = canvas.top()    - cur.top();
                else if (qAbs(cur.bottom() - canvas.bottom()) < snapPx) shiftY = canvas.bottom() - cur.bottom();
                else if (qAbs(cur.center().y() - canvas.center().y()) < snapPx)
                    shiftY = canvas.center().y() - cur.center().y();
                if (shiftX != 0.0 || shiftY != 0.0) {
                    m_videoSourceDx = qBound(-5.0, m_videoSourceDx + shiftX / lbw, 5.0);
                    m_videoSourceDy = qBound(-5.0, m_videoSourceDy + shiftY / lbh, 5.0);
                }
            }
            emit videoSourceTransformChanged(m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
            update();
            event->accept();
            return;
        }
        // Hover cursor for idle video transform selection.
        if (m_videoTransformSelected) {
            const TextToolHandle hit = hitTestVideoHandle(event->pos());
            if (hit != HandleNone) setCursor(Qt::SizeFDiagCursor);
            else if (pointInsideVideoRect(event->pos())) setCursor(Qt::SizeAllCursor);
            else setCursor(Qt::ArrowCursor);
        }
        QOpenGLWidget::mouseMoveEvent(event);
        return;
    }

    // Hover cursor feedback when idle. Handles take precedence so the user
    // can grab a corner while typing; inside-rect shows I-beam during edit.
    if (m_textToolMode == TextToolIdle) {
        const TextToolHandle hit = hitTestTextToolHandle(event->pos());
        if (hit == HandleTL || hit == HandleBR)        setCursor(Qt::SizeFDiagCursor);
        else if (hit == HandleTR || hit == HandleBL)   setCursor(Qt::SizeBDiagCursor);
        else if (hit == HandleT  || hit == HandleB)    setCursor(Qt::SizeVerCursor);
        else if (hit == HandleL  || hit == HandleR)    setCursor(Qt::SizeHorCursor);
        else if (pointInsideTextToolRect(event->pos()))
            setCursor(m_textToolEditing ? Qt::IBeamCursor : Qt::SizeAllCursor);
        else                                           setCursor(Qt::IBeamCursor);
        return;
    }

    m_textToolCurrentPos = event->pos();
    if (m_textToolMode == TextToolCreating) {
        // rect is computed directly in paintGL from press/current
    } else if (m_textToolMode == TextToolMoving) {
        const QPoint delta = event->pos() - m_textToolPressPos;
        m_textToolRect = m_textToolDragStartRect.translated(delta);
    } else if (m_textToolMode == TextToolResizing) {
        QRect r = m_textToolDragStartRect;
        const QPoint p = event->pos();
        switch (m_textToolActiveHandle) {
            case HandleTL: r.setTopLeft(p); break;
            case HandleT:  r.setTop(p.y()); break;
            case HandleTR: r.setTopRight(p); break;
            case HandleL:  r.setLeft(p.x()); break;
            case HandleR:  r.setRight(p.x()); break;
            case HandleBL: r.setBottomLeft(p); break;
            case HandleB:  r.setBottom(p.y()); break;
            case HandleBR: r.setBottomRight(p); break;
            default: break;
        }
        m_textToolRect = r.normalized();
    }
    update();
    event->accept();
}

void GLPreview::mouseReleaseEvent(QMouseEvent *event)
{
    // US-MOCHA-3: SurfaceTool handles mouse release when dragging a corner.
    if (m_surfaceTool && m_surfaceTool->isEnabled()) {
        if (m_surfaceTool->handleMouseRelease(event->pos(), event->button(), event->modifiers())) {
            event->accept();
            return;
        }
    }
    if (!m_textToolActive && m_videoDragMode != VideoDragNone) {
        m_videoDragMode = VideoDragNone;
        m_videoDragHandle = HandleNone;
        emit videoSourceTransformChanged(m_videoSourceScale, m_videoSourceDx, m_videoSourceDy);
        update();
        event->accept();
        return;
    }
    if (!m_textToolActive || event->button() != Qt::LeftButton
        || m_textToolMode == TextToolIdle) {
        QOpenGLWidget::mouseReleaseEvent(event);
        return;
    }
    const TextToolInteraction mode = m_textToolMode;
    m_textToolMode = TextToolIdle;
    m_textToolActiveHandle = HandleNone;
    event->accept();

    if (mode == TextToolCreating) {
        const QRect rawRect = QRect(m_textToolPressPos, event->pos()).normalized();
        // 8x8 minimum prevents accidental click-and-release from creating
        // an invisible text box.
        if (rawRect.width() < 8 || rawRect.height() < 8) {
            m_textToolHasRect = false;
            exitTextToolEditMode();
            update();
            return;
        }
        m_textToolRect = rawRect;
        m_textToolHasRect = true;
        // Adobe-style in-place editing: after a successful create-drag,
        // the widget enters edit mode and a blinking caret appears inside
        // the new rect so the user can immediately start typing.
        enterTextToolEditMode();
    }
    update();
    // Existing-overlay move/resize emits textOverlayRectChanged so MainWindow
    // rewrites the rect in place; everything else takes the generic path.
    if ((mode == TextToolMoving || mode == TextToolResizing) && m_textToolEditingIndex >= 0) {
        const QRectF lb = letterboxRect();
        const double lbw = qMax(1.0, lb.width());
        const double lbh = qMax(1.0, lb.height());
        const double nx = ((m_textToolRect.x() + m_textToolRect.width()  / 2.0) - lb.x()) / lbw;
        const double ny = ((m_textToolRect.y() + m_textToolRect.height() / 2.0) - lb.y()) / lbh;
        const double nw = m_textToolRect.width()  / lbw;
        const double nh = m_textToolRect.height() / lbh;
        emit textOverlayRectChanged(m_textToolEditingIndex, QRectF(nx, ny, nw, nh));
    } else {
        emitCurrentTextToolRect();
    }
}

void GLPreview::mouseDoubleClickEvent(QMouseEvent *event)
{
    // OBS-style edit mode: second click on an existing overlay enters inline
    // text editing with caret. Single click only selects (see mousePressEvent).
    if (!m_textToolActive || event->button() != Qt::LeftButton) {
        QOpenGLWidget::mouseDoubleClickEvent(event);
        return;
    }
    // Use the rect populated by the preceding single-click (or fall back to
    // a fresh overlay hit-test if the user somehow double-clicked without a
    // prior single-click landing on the same overlay).
    int idx = m_textToolEditingIndex;
    if (idx < 0 || !m_textToolHasRect || !m_textToolRect.contains(event->pos()))
        idx = hitTestExistingOverlay(event->pos());
    if (idx < 0) {
        QOpenGLWidget::mouseDoubleClickEvent(event);
        return;
    }
    QString prefill;
    for (const auto &h : m_textToolOverlayHits) {
        if (h.index == idx) { prefill = h.text; break; }
    }
    m_textToolEditingIndex = idx;
    enterTextToolEditMode();
    m_textToolInputText = prefill;
    update();
    event->accept();
}

void GLPreview::keyPressEvent(QKeyEvent *event)
{
    if (!m_textToolActive || !m_textToolEditing) {
        if (m_textToolActive && m_textToolHasRect
            && event->key() == Qt::Key_Escape) {
            // Deselect in OBS-style: Escape clears selection when not editing.
            m_textToolHasRect = false;
            m_textToolEditingIndex = -1;
            m_textToolMode = TextToolIdle;
            update();
            event->accept();
            return;
        }
        // US-T34 Video transform shortcuts (only when text tool off).
        if (!m_textToolActive && m_videoTransformSelected) {
            if (event->key() == Qt::Key_Escape) {
                m_videoTransformSelected = false;
                update();
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_0 || event->key() == Qt::Key_Delete) {
                resetVideoSourceTransform();
                event->accept();
                return;
            }
        }
        QOpenGLWidget::keyPressEvent(event);
        return;
    }
    const int key = event->key();
    if (key == Qt::Key_Escape) {
        exitTextToolEditMode();
        event->accept();
        return;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        // Delegate to the single commit path: it exits edit mode first
        // then emits the signal, breaking the applyTextToolOverlay recursion
        // that caused Enter-press crashes.
        commitCurrentTextToolEdit();
        event->accept();
        return;
    }
    if (key == Qt::Key_Backspace) {
        if (!m_textToolInputText.isEmpty())
            m_textToolInputText.chop(1);
        m_textToolCaretVisible = true;
        update();
        event->accept();
        return;
    }
    const QString typed = event->text();
    if (!typed.isEmpty() && typed.at(0).isPrint()) {
        m_textToolInputText.append(typed);
        m_textToolCaretVisible = true;
        update();
        event->accept();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void GLPreview::focusOutEvent(QFocusEvent *event)
{
    // Committing here would be surprising (the user might just be clicking
    // a different widget). Silently keep the text but freeze the caret;
    // they can re-focus and continue typing.
    m_textToolCaretVisible = false;
    m_textToolCaretTimer.stop();
    update();
    QOpenGLWidget::focusOutEvent(event);
}

void GLPreview::setLut(const LutData &lut)
{
    if (!lut.isValid()) {
        clearLut();
        return;
    }

    makeCurrent();

    if (m_lutTexture) {
        delete m_lutTexture;
        m_lutTexture = nullptr;
    }

    // Create 3D texture from LUT table
    m_lutTexture = new QOpenGLTexture(QOpenGLTexture::Target3D);
    m_lutTexture->setSize(lut.size, lut.size, lut.size);
    m_lutTexture->setFormat(QOpenGLTexture::RGB32F);
    m_lutTexture->allocateStorage();
    m_lutTexture->setMinificationFilter(QOpenGLTexture::Linear);
    m_lutTexture->setMagnificationFilter(QOpenGLTexture::Linear);
    m_lutTexture->setWrapMode(QOpenGLTexture::ClampToEdge);

    // Upload LUT data as float RGB
    QVector<float> data;
    data.reserve(lut.table.size() * 3);
    for (const QVector3D &v : lut.table) {
        data.append(v.x());
        data.append(v.y());
        data.append(v.z());
    }
    m_lutTexture->setData(QOpenGLTexture::RGB, QOpenGLTexture::Float32,
                          data.constData());

    m_lutIntensity = static_cast<float>(lut.intensity);
    m_lutEnabled = true;

    doneCurrent();
    update();
}

// US-FEAT-B: LUT 3D-texture blend — upload QImage grid as GL_TEXTURE_3D
// with auto-detection of cube layout (32^3, 64^3 horizontal/vertical strip, HALD).
void GLPreview::setLutTexture(const QImage &lutGrid, float intensity)
{
    if (lutGrid.isNull())
        return;

    const int w = lutGrid.width();
    const int h = lutGrid.height();
    int cubeSize = 0;
    bool horizontal = true; // true = N*N wide × N tall (HALD), false = N wide × N*N tall

    // Auto-detect cube size from grid dimensions
    if (w == 32 && h == 1024)        { cubeSize = 32; horizontal = false; }
    else if (w == 1024 && h == 32)   { cubeSize = 32; horizontal = true; }
    else if (w == 64 && h == 4096)   { cubeSize = 64; horizontal = false; }
    else if (w == 4096 && h == 64)   { cubeSize = 64; horizontal = true; }
    else {
        qWarning() << "GLPreview::setLutTexture: unsupported LUT grid size" << w << "x" << h;
        return;
    }

    // Convert to 8-bit RGBA for consistent pixel access
    QImage img = lutGrid.convertToFormat(QImage::Format_RGBA8888);

    // Build 3D float-RGB data: cubeSize × cubeSize × cubeSize × 3
    QVector<float> data;
    data.resize(cubeSize * cubeSize * cubeSize * 3);

    for (int b = 0; b < cubeSize; ++b) {
        for (int g = 0; g < cubeSize; ++g) {
            for (int r = 0; r < cubeSize; ++r) {
                int px, py;
                if (horizontal) {
                    // HALD layout: width = cubeSize*cubeSize, height = cubeSize
                    px = g * cubeSize + r;
                    py = b;
                } else {
                    // Transposed: width = cubeSize, height = cubeSize*cubeSize
                    px = b;
                    py = g * cubeSize + r;
                }
                const QRgb pixel = img.pixel(px, py);
                const int idx = (b * cubeSize * cubeSize + g * cubeSize + r) * 3;
                data[idx + 0] = qRed(pixel)   / 255.0f;
                data[idx + 1] = qGreen(pixel) / 255.0f;
                data[idx + 2] = qBlue(pixel)  / 255.0f;
            }
        }
    }

    makeCurrent();

    if (m_lutTexture) {
        delete m_lutTexture;
        m_lutTexture = nullptr;
    }

    m_lutTexture = new QOpenGLTexture(QOpenGLTexture::Target3D);
    m_lutTexture->setSize(cubeSize, cubeSize, cubeSize);
    m_lutTexture->setFormat(QOpenGLTexture::RGB32F);
    m_lutTexture->allocateStorage();
    m_lutTexture->setMinificationFilter(QOpenGLTexture::Linear);
    m_lutTexture->setMagnificationFilter(QOpenGLTexture::Linear);
    m_lutTexture->setWrapMode(QOpenGLTexture::ClampToEdge);
    m_lutTexture->setData(QOpenGLTexture::RGB, QOpenGLTexture::Float32,
                          data.constData());

    m_lutIntensity = qBound(0.0f, intensity, 1.0f);
    m_lutEnabled = true;

    doneCurrent();
    update();
}

void GLPreview::setLutIntensity(double intensity)
{
    m_lutIntensity = qBound(0.0f, static_cast<float>(intensity), 1.0f);
    update();
}

// US-CG-1: queue an RGB curves upload. The actual GL texture upload runs
// inside paintGL because that's where we know the GL context is current.
// Identity curves (every entry == its index) effectively no-op the shader
// stage, so we still upload them — that lets users toggle channels back
// and forth without re-creating the texture each time.
void GLPreview::setRgbCurves(const QVector<QVector<int>> &curves)
{
    if (curves.size() < 4) return;
    m_pendingCurves = curves;
    m_curvesNeedUpload = true;
    m_curvesEnabled = true;
    update();
}

// US-CG-4: queue a Hue vs Saturation LUT upload. The 256-float LUT is
// stored as-is; the actual GL texture upload runs inside paintGL where the
// GL context is guaranteed current. An empty / wrong-sized vector disables
// the stage so the shader becomes a free no-op.
void GLPreview::setHueVsSatLut(const QVector<float> &lut)
{
    if (lut.size() != 256) {
        m_hueVsSatEnabled = false;
        update();
        return;
    }
    m_pendingHueVsSatLut = lut;
    m_hueVsSatNeedUpload = true;
    m_hueVsSatEnabled = true;
    update();
}

// US-CG-2: White-balance gain triple. Stored on the widget; uploaded as the
// uWb uniform on the next paint. Identity is (1, 1, 1).
void GLPreview::setWhiteBalance(float r, float g, float b)
{
    m_wb[0] = r;
    m_wb[1] = g;
    m_wb[2] = b;
    update();
}

// US-CG-3: Radial vignette / Power Window. Stored on the widget; uploaded as
// uVigAmount/uVigMid/uVigRound/uVigFeather on the next paint. Identity is
// amount==0 (factor==1.0 in the shader → free no-op).
void GLPreview::setVignette(float amount, float midpoint, float roundness, float feather)
{
    m_vigAmount  = amount;
    m_vigMid     = midpoint;
    m_vigRound   = roundness;
    m_vigFeather = feather;
    update();
}

// US-EF-1: Chroma Key. Stored on the widget; uploaded as uChroma* uniforms
// on the next paint. enabled=false is a free no-op (the entire shader stage
// is skipped). All scalar inputs are already normalized to [0..1] by the
// ColorGradingPanel before they reach us.
void GLPreview::setChromaKey(bool enabled, float keyH, float keyS, float keyL,
                             float hueTol, float satTol, float lumTol,
                             float spill, float softness)
{
    m_chromaEnabled        = enabled;
    m_chromaKeyH           = keyH;
    m_chromaKeyS           = keyS;
    m_chromaKeyL           = keyL;
    m_chromaHueTol         = hueTol;
    m_chromaSatTol         = satTol;
    m_chromaLumTol         = lumTol;
    m_chromaSpillStrength  = spill;
    m_chromaSoftness       = softness;
    update();
}

// US-EF-2: Mask Animation (DaVinci Power Window simplified). Stored on the
// widget; uploaded as uMask* uniforms on the next paint. enabled=false is a
// free no-op (the entire wrap stage is skipped in the fragment shader).
void GLPreview::setMask(bool enabled, bool ellipse, bool invert, float feather,
                        QRectF normalizedRect)
{
    m_maskEnabled = enabled;
    m_maskEllipse = ellipse;
    m_maskInvert  = invert;
    m_maskFeather = feather;
    // Defensive clamp — guarantees the shader sees finite, in-range values
    // even if the upstream emitter glitches.
    m_maskRect[0] = static_cast<float>(std::clamp(normalizedRect.x(), 0.0, 1.0));
    m_maskRect[1] = static_cast<float>(std::clamp(normalizedRect.y(), 0.0, 1.0));
    m_maskRect[2] = static_cast<float>(std::clamp(normalizedRect.width(),
                                                  0.001, 1.0));
    m_maskRect[3] = static_cast<float>(std::clamp(normalizedRect.height(),
                                                  0.001, 1.0));
    update();
}

// US-EF-3: HSL Qualifier (DaVinci secondary grading). Stored on the widget;
// uploaded as uHslq* uniforms on the next paint. Sits AFTER chroma key and
// BEFORE WB so the qualifier acts on raw frame colour. enabled=false is a
// free no-op (the entire shader stage is skipped). Inputs:
//   hueCenter   degrees [0..360]
//   hueRange    degrees [0..180]
//   satMin/Max  fractions [0..1] (panel divides percent by 100)
//   lumaMin/Max fractions [0..1]
//   softness    [0..50] degrees / percent edge slop
//   lift / gamma / gain   per-channel adjustment (identity 0/1/1)
void GLPreview::setHslQualifier(bool enabled,
                                float hueCenter, float hueRange,
                                float satMin, float satMax,
                                float lumaMin, float lumaMax,
                                float softness,
                                float liftR, float liftG, float liftB,
                                float gammaR, float gammaG, float gammaB,
                                float gainR, float gainG, float gainB)
{
    m_hslqEnabled   = enabled;
    m_hslqHueCenter = hueCenter;
    m_hslqHueRange  = hueRange;
    m_hslqSatMin    = satMin;
    m_hslqSatMax    = satMax;
    m_hslqLumaMin   = lumaMin;
    m_hslqLumaMax   = lumaMax;
    m_hslqSoftness  = softness;
    m_hslqLift[0]   = liftR;
    m_hslqLift[1]   = liftG;
    m_hslqLift[2]   = liftB;
    m_hslqGamma[0]  = gammaR;
    m_hslqGamma[1]  = gammaG;
    m_hslqGamma[2]  = gammaB;
    m_hslqGain[0]   = gainR;
    m_hslqGain[1]   = gainG;
    m_hslqGain[2]   = gainB;
    update();
}

// US-EF-4: Effects shader pack — Sharpen / Gaussian Blur / Lens Distortion.
// Each setter just stores the panel-emitted scalar and triggers a repaint;
// the shader's |amount|>eps tests keep identity (0) a free no-op.
void GLPreview::setSharpen(float amount)
{
    m_sharpenAmount = amount;
    update();
}

void GLPreview::setBlur(float radiusPx)
{
    m_blurRadius = radiusPx;
    update();
}

void GLPreview::setLensDistortion(float amount)
{
    m_lensDistortion = amount;
    update();
}

// US-3D: 3-axis rotation + perspective foreshortening. Build the 3x3
// rotation matrix from intrinsic Tait-Bryan XYZ Euler angles
// (R = Rx * Ry * Rz) on the CPU side and ship it to the fragment shader as
// a QMatrix3x3. perspectiveDist is the inverse FOV — clamped to [0.1..10.0]
// so the shader's perspective divide cannot go singular. Identity (all
// angles 0, perspectiveDist any value) is detected with a tight epsilon
// and turns the stage into a free no-op.
void GLPreview::setRotation3D(float xDeg, float yDeg, float zDeg, float perspectiveDist)
{
    constexpr float kPi = 3.14159265358979323846f;
    const float xRad = xDeg * kPi / 180.0f;
    const float yRad = yDeg * kPi / 180.0f;
    const float zRad = zDeg * kPi / 180.0f;

    const float cx = std::cos(xRad), sx = std::sin(xRad);
    const float cy = std::cos(yRad), sy = std::sin(yRad);
    const float cz = std::cos(zRad), sz = std::sin(zRad);

    // R = Rx * Ry * Rz (intrinsic Tait-Bryan). Standard rotation matrices.
    //   Rx = [1 0 0; 0 cx -sx; 0 sx cx]
    //   Ry = [cy 0 sy; 0 1 0; -sy 0 cy]
    //   Rz = [cz -sz 0; sz cz 0; 0 0 1]
    // Composed product written out so we don't pull in QMatrix4x4 multiply.
    // QMatrix3x3(const float*) expects row-major values.
    m_rot3DMatrix[0] = cy * cz;
    m_rot3DMatrix[1] = -cy * sz;
    m_rot3DMatrix[2] = sy;
    m_rot3DMatrix[3] = sx * sy * cz + cx * sz;
    m_rot3DMatrix[4] = -sx * sy * sz + cx * cz;
    m_rot3DMatrix[5] = -sx * cy;
    m_rot3DMatrix[6] = -cx * sy * cz + sx * sz;
    m_rot3DMatrix[7] = cx * sy * sz + sx * cz;
    m_rot3DMatrix[8] = cx * cy;

    m_perspectiveDist = std::max(0.1f, std::min(10.0f, perspectiveDist));

    // Identity check: any axis with non-trivial rotation enables the stage.
    // 0.05° threshold matches the slider's integer-degree resolution so a
    // dead-centre slider produces a true no-op.
    constexpr float kIdentityThreshold = 0.05f;
    m_rot3DEnabled = (std::abs(xDeg) > kIdentityThreshold
                   || std::abs(yDeg) > kIdentityThreshold
                   || std::abs(zDeg) > kIdentityThreshold);
    update();
}

// US-INT-4: stabilizer keyframe ingest. Vector is copied; caller-supplied
// keyframes are assumed sorted by timeUs (analyzeFile produces them in
// frame-decode order which is monotonic). Empty vector disables the stage.
void GLPreview::setStabilizerKeyframes(const QVector<StabilizerKeyframe> &kfs)
{
    m_stabKeyframes = kfs;
    if (m_stabKeyframes.isEmpty()) {
        m_stabEnabled = false;
        m_stabMatrix[0] = 1.0f; m_stabMatrix[1] = 0.0f; m_stabMatrix[2] = 0.0f;
        m_stabMatrix[3] = 0.0f; m_stabMatrix[4] = 1.0f; m_stabMatrix[5] = 0.0f;
        m_stabMatrix[6] = 0.0f; m_stabMatrix[7] = 0.0f; m_stabMatrix[8] = 1.0f;
    }
    update();
}

// US-INT-4: per-frame source-time hook. std::lower_bound picks the first
// keyframe with timeUs >= sourceUs; we lerp between (idx-1) and idx for a
// smooth inverse-affine. Out-of-range source times clamp to the endpoints.
// Builds the inverse 2D affine in (lensCoord-0.5) space:
//   inverse stab = scale(1/s) * rotate(-theta) * translate(-dx_uv, -dy_uv)
// where dx_uv/dy_uv are the keyframe pixel offsets divided by frame size
// (frame size = MotionStabilizer's last source dim, captured below). The
// resulting 3x3 mat3 is stored row-major in m_stabMatrix.
void GLPreview::setStabilizerSourceTimeUs(qint64 sourceUs)
{
    m_stabSourceUs = sourceUs;
    if (m_stabKeyframes.isEmpty()) {
        m_stabEnabled = false;
        return;
    }

    // Binary search for the first keyframe with timeUs >= sourceUs.
    auto cmp = [](const StabilizerKeyframe &kf, qint64 t) { return kf.timeUs < t; };
    auto it = std::lower_bound(m_stabKeyframes.constBegin(), m_stabKeyframes.constEnd(),
                               sourceUs, cmp);
    int idx = static_cast<int>(it - m_stabKeyframes.constBegin());

    double dx = 0.0, dy = 0.0, theta = 0.0, scl = 1.0;
    if (idx <= 0) {
        const auto &kf = m_stabKeyframes.first();
        dx = kf.dx; dy = kf.dy; theta = kf.theta; scl = kf.scale;
    } else if (idx >= m_stabKeyframes.size()) {
        const auto &kf = m_stabKeyframes.last();
        dx = kf.dx; dy = kf.dy; theta = kf.theta; scl = kf.scale;
    } else {
        const auto &a = m_stabKeyframes[idx - 1];
        const auto &b = m_stabKeyframes[idx];
        const qint64 span = b.timeUs - a.timeUs;
        const double t = (span > 0)
            ? static_cast<double>(sourceUs - a.timeUs) / static_cast<double>(span)
            : 0.0;
        dx    = a.dx    + (b.dx    - a.dx)    * t;
        dy    = a.dy    + (b.dy    - a.dy)    * t;
        theta = a.theta + (b.theta - a.theta) * t;
        scl   = a.scale + (b.scale - a.scale) * t;
    }

    // Frame size: MotionStabilizer trans deltas are in SOURCE pixels; the
    // shader pre-warp operates in normalised (0..1) UV space. Divide by
    // current frame dimensions (set by setFrameSize / displayFrame). Fall
    // back to 1920x1080 if unknown so we don't divide by zero.
    const double fw = (m_stabFrameW > 0) ? static_cast<double>(m_stabFrameW) : 1920.0;
    const double fh = (m_stabFrameH > 0) ? static_cast<double>(m_stabFrameH) : 1080.0;
    const double dxUv = dx / fw;
    const double dyUv = dy / fh;

    // Inverse 2D affine in (cx, cy, 1) space:
    //   c' = (1/s) * R(-theta) * c - (dxUv, dyUv)
    // where R(-theta) = [ cosT  sinT; -sinT  cosT ] (note flipped signs vs R(theta))
    const double invS = (std::abs(scl) > 1e-9) ? 1.0 / scl : 1.0;
    const double cT = std::cos(-theta);
    const double sT = std::sin(-theta);

    // Row-major 3x3 matrix [m11 m12 m13; m21 m22 m23; 0 0 1].
    // q.x = invS*(cT*cx + sT*cy) - dxUv
    // q.y = invS*(-sT*cx + cT*cy) - dyUv
    m_stabMatrix[0] = static_cast<float>(invS * cT);
    m_stabMatrix[1] = static_cast<float>(invS * sT);
    m_stabMatrix[2] = static_cast<float>(-dxUv);
    m_stabMatrix[3] = static_cast<float>(invS * -sT);
    m_stabMatrix[4] = static_cast<float>(invS * cT);
    m_stabMatrix[5] = static_cast<float>(-dyUv);
    m_stabMatrix[6] = 0.0f;
    m_stabMatrix[7] = 0.0f;
    m_stabMatrix[8] = 1.0f;

    constexpr double kIdEps = 1e-6;
    m_stabEnabled = (std::abs(dxUv) > kIdEps
                  || std::abs(dyUv) > kIdEps
                  || std::abs(theta) > kIdEps
                  || std::abs(scl - 1.0) > kIdEps);
    update();
}

void GLPreview::setLiftGammaGain(const std::array<std::array<double,4>,3> &values)
{
    // values[0] = Lift:  panel stores slider/100  [-1,1], neutral=0 → scale to [-0.5, 0.5]
    // values[1] = Gamma: panel stores 0.1*pow(40,t) [~0.1,4], neutral≈1 → pass through
    // values[2] = Gain:  panel stores slider/100  [-1,1], neutral=0 → convert to 2^(v*2) [0.25,4]
    for (int ch = 0; ch < 4; ++ch) {
        m_liftGammaGain[0][ch] = values[0][ch] * 0.5;
        m_liftGammaGain[1][ch] = values[1][ch];
        m_liftGammaGain[2][ch] = std::pow(2.0, values[2][ch] * 2.0);
    }
    update();
}

void GLPreview::clearLut()
{
    makeCurrent();
    if (m_lutTexture) {
        delete m_lutTexture;
        m_lutTexture = nullptr;
    }
    m_lutEnabled = false;
    // US-FEAT-B: zero intensity so LUT has no residual effect if re-enabled
    m_lutIntensity = 0.0f;
    doneCurrent();
    update();
}

// US-MOCHA-3: install a SurfaceTool for 4-corner planar tracking gizmo.
void GLPreview::installSurfaceTool(SurfaceTool *tool)
{
    m_surfaceTool = tool;
    update();
}
