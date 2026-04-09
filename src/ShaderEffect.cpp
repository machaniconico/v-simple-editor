#include "ShaderEffect.h"
#include <QDebug>
#include <QOpenGLContext>
#include <utility>

// ---------------------------------------------------------------------------
// Shared vertex shader — fullscreen quad, same as GLPreview
// ---------------------------------------------------------------------------

const char *ShaderEffectRenderer::s_vertexShaderSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

// ===========================================================================
// Built-in fragment shaders
// ===========================================================================

// ---------------------------------------------------------------------------
// Category: Color
// ---------------------------------------------------------------------------

static const char *kFragChromaticAberration = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uAmount;   // 0.0 - 0.05, default 0.01

void main() {
    vec2 dir = vTexCoord - vec2(0.5);
    float dist = length(dir);
    vec2 offset = normalize(dir) * dist * uAmount;
    float r = texture(uTexture, vTexCoord + offset).r;
    float g = texture(uTexture, vTexCoord).g;
    float b = texture(uTexture, vTexCoord - offset).b;
    float a = texture(uTexture, vTexCoord).a;
    fragColor = vec4(r, g, b, a);
}
)";

static const char *kFragColorHalftone = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uDotSize;    // 4.0 - 20.0, default 8.0
uniform vec2 uResolution;  // output size in pixels

void main() {
    vec2 pixel = vTexCoord * uResolution;
    // CMYK angles: C=15, M=75, Y=90, K=45 degrees
    const float angles[4] = float[4](0.2618, 1.3090, 1.5708, 0.7854);

    vec4 src = texture(uTexture, vTexCoord);
    // Convert RGB to CMYK
    float k = 1.0 - max(src.r, max(src.g, src.b));
    float c = k < 1.0 ? (1.0 - src.r - k) / (1.0 - k) : 0.0;
    float m = k < 1.0 ? (1.0 - src.g - k) / (1.0 - k) : 0.0;
    float y = k < 1.0 ? (1.0 - src.b - k) / (1.0 - k) : 0.0;

    float cmyk[4] = float[4](c, m, y, k);
    float result[4] = float[4](0.0, 0.0, 0.0, 0.0);

    for (int i = 0; i < 4; i++) {
        float s = cos(angles[i]);
        float cs = sin(angles[i]);
        vec2 rotPixel = vec2(pixel.x * s - pixel.y * cs,
                             pixel.x * cs + pixel.y * s);
        vec2 cell = fract(rotPixel / uDotSize) - 0.5;
        float d = length(cell);
        float radius = cmyk[i] * 0.5;
        result[i] = d < radius ? 1.0 : 0.0;
    }

    // Composite: subtract ink channels from white
    float r = 1.0 - result[0] - result[3];
    float g = 1.0 - result[1] - result[3];
    float b = 1.0 - result[2] - result[3];
    fragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), src.a);
}
)";

static const char *kFragDuotone = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform vec3 uColorShadow;  // dark tone color, default vec3(0.1, 0.0, 0.2)
uniform vec3 uColorHighlight; // light tone color, default vec3(1.0, 0.9, 0.2)

void main() {
    vec4 src = texture(uTexture, vTexCoord);
    float lum = dot(src.rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3 color = mix(uColorShadow, uColorHighlight, lum);
    fragColor = vec4(color, src.a);
}
)";

static const char *kFragGradientMap = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
// 5-stop gradient: each stop is a vec3 RGB, evenly spaced 0..1
uniform vec3 uStop0;  // default vec3(0.0, 0.0, 0.0)
uniform vec3 uStop1;  // default vec3(0.2, 0.0, 0.5)
uniform vec3 uStop2;  // default vec3(0.8, 0.1, 0.3)
uniform vec3 uStop3;  // default vec3(1.0, 0.6, 0.0)
uniform vec3 uStop4;  // default vec3(1.0, 1.0, 0.9)

void main() {
    vec4 src = texture(uTexture, vTexCoord);
    float lum = dot(src.rgb, vec3(0.2126, 0.7152, 0.0722));

    vec3 color;
    if (lum < 0.25) {
        color = mix(uStop0, uStop1, lum / 0.25);
    } else if (lum < 0.5) {
        color = mix(uStop1, uStop2, (lum - 0.25) / 0.25);
    } else if (lum < 0.75) {
        color = mix(uStop2, uStop3, (lum - 0.5) / 0.25);
    } else {
        color = mix(uStop3, uStop4, (lum - 0.75) / 0.25);
    }

    fragColor = vec4(color, src.a);
}
)";

// ---------------------------------------------------------------------------
// Category: Blur
// ---------------------------------------------------------------------------

static const char *kFragGaussianBlurH = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uRadius;      // 1.0 - 30.0, default 5.0
uniform vec2 uResolution;

void main() {
    float sigma = max(uRadius, 0.1);
    int kernelSize = int(ceil(sigma * 3.0));
    vec2 texelSize = 1.0 / uResolution;

    vec4 result = vec4(0.0);
    float weightSum = 0.0;

    for (int i = -kernelSize; i <= kernelSize; i++) {
        float weight = exp(-float(i * i) / (2.0 * sigma * sigma));
        vec2 offset = vec2(float(i) * texelSize.x, 0.0);
        result += texture(uTexture, vTexCoord + offset) * weight;
        weightSum += weight;
    }

    fragColor = result / weightSum;
}
)";

static const char *kFragGaussianBlurV = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uRadius;
uniform vec2 uResolution;

void main() {
    float sigma = max(uRadius, 0.1);
    int kernelSize = int(ceil(sigma * 3.0));
    vec2 texelSize = 1.0 / uResolution;

    vec4 result = vec4(0.0);
    float weightSum = 0.0;

    for (int i = -kernelSize; i <= kernelSize; i++) {
        float weight = exp(-float(i * i) / (2.0 * sigma * sigma));
        vec2 offset = vec2(0.0, float(i) * texelSize.y);
        result += texture(uTexture, vTexCoord + offset) * weight;
        weightSum += weight;
    }

    fragColor = result / weightSum;
}
)";

static const char *kFragRadialBlur = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uStrength;   // 0.0 - 0.1, default 0.02
uniform vec2  uCenter;     // 0.0-1.0, default vec2(0.5, 0.5)
uniform int   uSamples;    // 8 - 32, default 16

void main() {
    vec2 dir = vTexCoord - uCenter;
    vec4 result = vec4(0.0);
    int samples = clamp(uSamples, 1, 32);
    for (int i = 0; i < samples; i++) {
        float t = float(i) / float(samples);
        vec2 sampleUV = vTexCoord - dir * uStrength * t;
        result += texture(uTexture, sampleUV);
    }
    fragColor = result / float(samples);
}
)";

static const char *kFragDirectionalBlur = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uAngle;      // degrees, 0.0 - 360.0, default 0.0
uniform float uStrength;   // 0.0 - 0.05, default 0.01
uniform int   uSamples;    // 8 - 32, default 16
uniform vec2  uResolution;

void main() {
    float rad = radians(uAngle);
    vec2 dir = vec2(cos(rad), sin(rad)) * uStrength;
    vec4 result = vec4(0.0);
    int samples = clamp(uSamples, 1, 32);
    for (int i = -samples / 2; i <= samples / 2; i++) {
        float t = float(i) / float(samples);
        result += texture(uTexture, vTexCoord + dir * t);
    }
    fragColor = result / float(samples + 1);
}
)";

static const char *kFragTiltShift = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uFocusCenter;  // 0.0 - 1.0, default 0.5 (Y position of focus band)
uniform float uFocusRange;   // 0.0 - 0.5, default 0.1
uniform float uBlurRadius;   // 1.0 - 20.0, default 8.0
uniform vec2  uResolution;

void main() {
    float dist = abs(vTexCoord.y - uFocusCenter);
    float blur = smoothstep(0.0, uFocusRange, dist - uFocusRange) * uBlurRadius;

    if (blur < 0.5) {
        fragColor = texture(uTexture, vTexCoord);
        return;
    }

    float sigma = blur;
    int kernelSize = int(ceil(sigma * 2.5));
    vec2 texelSize = 1.0 / uResolution;

    vec4 result = vec4(0.0);
    float weightSum = 0.0;

    for (int i = -kernelSize; i <= kernelSize; i++) {
        for (int j = -kernelSize; j <= kernelSize; j++) {
            float w = exp(-float(i*i + j*j) / (2.0 * sigma * sigma));
            vec2 offset = vec2(float(i) * texelSize.x, float(j) * texelSize.y);
            result += texture(uTexture, vTexCoord + offset) * w;
            weightSum += w;
        }
    }

    fragColor = result / weightSum;
}
)";

// ---------------------------------------------------------------------------
// Category: Distort
// ---------------------------------------------------------------------------

static const char *kFragBarrelDistortion = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uK1;   // barrel/pincushion: -0.5 to 0.5, default -0.2
uniform float uK2;   // secondary term: -0.2 to 0.2, default 0.05

void main() {
    vec2 uv = vTexCoord * 2.0 - 1.0;  // to [-1, 1]
    float r2 = dot(uv, uv);
    float distort = 1.0 + uK1 * r2 + uK2 * r2 * r2;
    vec2 distorted = uv * distort;
    vec2 sampleUV = distorted * 0.5 + 0.5;

    if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        fragColor = texture(uTexture, sampleUV);
    }
}
)";

static const char *kFragRipple = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uTime;       // animated time, seconds
uniform float uAmplitude;  // 0.0 - 0.05, default 0.01
uniform float uFrequency;  // 5.0 - 30.0, default 12.0
uniform float uSpeed;      // 0.5 - 5.0, default 2.0

void main() {
    vec2 uv = vTexCoord;
    float wave = sin(uv.x * uFrequency + uTime * uSpeed) * uAmplitude;
    float wave2 = cos(uv.y * uFrequency * 0.8 + uTime * uSpeed * 1.2) * uAmplitude;
    uv.y += wave;
    uv.x += wave2;
    fragColor = texture(uTexture, clamp(uv, 0.0, 1.0));
}
)";

static const char *kFragPixelate = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uBlockSize;  // 2.0 - 64.0, default 8.0
uniform vec2  uResolution;

void main() {
    vec2 pixelSize = uBlockSize / uResolution;
    vec2 snapped = floor(vTexCoord / pixelSize) * pixelSize + pixelSize * 0.5;
    fragColor = texture(uTexture, snapped);
}
)";

static const char *kFragGlitch = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uTime;        // animated time
uniform float uIntensity;   // 0.0 - 1.0, default 0.3
uniform float uScanlines;   // 0.0 - 1.0, default 0.4
uniform vec2  uResolution;

float hash(float n) { return fract(sin(n) * 43758.5453); }
float hash2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    vec2 uv = vTexCoord;

    // Horizontal block glitch
    float blockY = floor(uv.y * 20.0);
    float glitchTime = floor(uTime * 4.0);
    float rnd = hash(blockY * 123.456 + glitchTime);
    if (rnd > 1.0 - uIntensity * 0.3) {
        uv.x += (hash(blockY + glitchTime) - 0.5) * 0.1 * uIntensity;
    }

    // RGB channel split
    float splitAmt = uIntensity * 0.015;
    float r = texture(uTexture, vec2(uv.x + splitAmt, uv.y)).r;
    float g = texture(uTexture, uv).g;
    float b = texture(uTexture, vec2(uv.x - splitAmt, uv.y)).b;
    float a = texture(uTexture, uv).a;

    vec3 color = vec3(r, g, b);

    // Scanlines
    float scanline = sin(uv.y * uResolution.y * 3.14159) * 0.5 + 0.5;
    color *= mix(1.0, scanline * 0.7 + 0.3, uScanlines);

    // Random noise blocks
    float noiseRnd = hash2(vec2(floor(uv.x * 40.0), floor(uv.y * 30.0)) + glitchTime);
    if (noiseRnd > 1.0 - uIntensity * 0.05) {
        color = vec3(hash(noiseRnd + 0.1), hash(noiseRnd + 0.2), hash(noiseRnd + 0.3));
    }

    fragColor = vec4(clamp(color, 0.0, 1.0), a);
}
)";

// ---------------------------------------------------------------------------
// Category: Stylize
// ---------------------------------------------------------------------------

static const char *kFragFilmGrain = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uAmount;   // 0.0 - 0.5, default 0.08
uniform float uTime;     // animated seed

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    vec4 src = texture(uTexture, vTexCoord);

    // Temporally varying noise
    float grain = hash(vTexCoord + vec2(floor(uTime * 24.0) * 0.01));
    grain = (grain - 0.5) * uAmount * 2.0;

    // More grain in midtones, less in shadows/highlights
    float lum = dot(src.rgb, vec3(0.2126, 0.7152, 0.0722));
    float grainMask = 4.0 * lum * (1.0 - lum);
    grain *= grainMask;

    fragColor = vec4(clamp(src.rgb + grain, 0.0, 1.0), src.a);
}
)";

static const char *kFragVignette = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uIntensity;  // 0.0 - 1.0, default 0.5
uniform float uRadius;     // 0.3 - 1.0, default 0.7
uniform float uSoftness;   // 0.1 - 0.8, default 0.4

void main() {
    vec4 src = texture(uTexture, vTexCoord);
    vec2 uv = vTexCoord - vec2(0.5);
    uv.x *= 1.2; // slight horizontal squeeze for natural feel
    float dist = length(uv);
    float vignette = smoothstep(uRadius, uRadius - uSoftness, dist);
    vignette = mix(1.0 - uIntensity, 1.0, vignette);
    fragColor = vec4(src.rgb * vignette, src.a);
}
)";

static const char *kFragCRT = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uScanlineIntensity;  // 0.0 - 1.0, default 0.4
uniform float uCurvature;          // 0.0 - 0.5, default 0.08
uniform float uBloom;              // 0.0 - 0.5, default 0.1
uniform vec2  uResolution;

void main() {
    // Barrel curvature
    vec2 uv = vTexCoord * 2.0 - 1.0;
    float r2 = dot(uv, uv);
    uv *= 1.0 + uCurvature * r2;
    vec2 sampleUV = uv * 0.5 + 0.5;

    // Edge blackout
    if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec4 src = texture(uTexture, sampleUV);
    vec3 color = src.rgb;

    // Scanlines
    float scanline = sin(sampleUV.y * uResolution.y * 3.14159) * 0.5 + 0.5;
    color *= mix(1.0, scanline * 0.6 + 0.4, uScanlineIntensity);

    // Bloom — add blurred version
    if (uBloom > 0.0) {
        vec2 texel = 1.0 / uResolution;
        vec3 bloom = vec3(0.0);
        for (int i = -2; i <= 2; i++) {
            for (int j = -2; j <= 2; j++) {
                bloom += texture(uTexture, sampleUV + vec2(float(i), float(j)) * texel * 3.0).rgb;
            }
        }
        bloom /= 25.0;
        color += bloom * uBloom;
    }

    // Phosphor color tint (slight green warmth)
    color *= vec3(0.95, 1.0, 0.9);

    // Vignette
    vec2 vigUV = sampleUV - 0.5;
    float vignette = 1.0 - dot(vigUV, vigUV) * 1.2;
    color *= clamp(vignette, 0.0, 1.0);

    fragColor = vec4(clamp(color, 0.0, 1.0), src.a);
}
)";

static const char *kFragSketch = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform float uEdgeStrength;  // 0.5 - 5.0, default 2.0
uniform float uLineThickness; // 0.5 - 3.0, default 1.0
uniform vec2  uResolution;

void main() {
    vec2 texel = uLineThickness / uResolution;

    // Sobel edge detection
    float tl = dot(texture(uTexture, vTexCoord + vec2(-texel.x, -texel.y)).rgb, vec3(0.299, 0.587, 0.114));
    float t  = dot(texture(uTexture, vTexCoord + vec2(     0.0, -texel.y)).rgb, vec3(0.299, 0.587, 0.114));
    float tr = dot(texture(uTexture, vTexCoord + vec2( texel.x, -texel.y)).rgb, vec3(0.299, 0.587, 0.114));
    float ml = dot(texture(uTexture, vTexCoord + vec2(-texel.x,      0.0)).rgb, vec3(0.299, 0.587, 0.114));
    float mr = dot(texture(uTexture, vTexCoord + vec2( texel.x,      0.0)).rgb, vec3(0.299, 0.587, 0.114));
    float bl = dot(texture(uTexture, vTexCoord + vec2(-texel.x,  texel.y)).rgb, vec3(0.299, 0.587, 0.114));
    float b  = dot(texture(uTexture, vTexCoord + vec2(     0.0,  texel.y)).rgb, vec3(0.299, 0.587, 0.114));
    float br = dot(texture(uTexture, vTexCoord + vec2( texel.x,  texel.y)).rgb, vec3(0.299, 0.587, 0.114));

    float gx = -tl - 2.0*ml - bl + tr + 2.0*mr + br;
    float gy = -tl - 2.0*t  - tr + bl + 2.0*b  + br;
    float edge = sqrt(gx*gx + gy*gy) * uEdgeStrength;
    edge = clamp(edge, 0.0, 1.0);

    // White background with dark pencil edges
    vec3 color = vec3(1.0) - vec3(edge);
    // Add slight paper texture via the original image luminance
    vec4 src = texture(uTexture, vTexCoord);
    float lum = dot(src.rgb, vec3(0.299, 0.587, 0.114));
    color = mix(color, vec3(lum * 0.15 + 0.85), 0.3);

    float alpha = texture(uTexture, vTexCoord).a;
    fragColor = vec4(clamp(color, 0.0, 1.0), alpha);
}
)";

static const char *kFragOilPaint = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
uniform int   uRadius;     // 2 - 6, default 4  (Kuwahara filter radius)
uniform vec2  uResolution;

// Kuwahara filter approximation — computes mean/variance in 4 quadrants
void main() {
    vec2 texel = 1.0 / uResolution;
    int r = clamp(uRadius, 1, 6);

    vec3 mean[4];
    float variance[4];

    for (int q = 0; q < 4; q++) {
        mean[q] = vec3(0.0);
        variance[q] = 0.0;
    }

    float count[4] = float[4](0.0, 0.0, 0.0, 0.0);

    for (int j = -r; j <= 0; j++) {
        for (int i = -r; i <= 0; i++) {
            vec3 c = texture(uTexture, vTexCoord + vec2(float(i), float(j)) * texel).rgb;
            mean[0] += c; count[0] += 1.0;
        }
        for (int i = 0; i <= r; i++) {
            vec3 c = texture(uTexture, vTexCoord + vec2(float(i), float(j)) * texel).rgb;
            mean[1] += c; count[1] += 1.0;
        }
    }
    for (int j = 0; j <= r; j++) {
        for (int i = -r; i <= 0; i++) {
            vec3 c = texture(uTexture, vTexCoord + vec2(float(i), float(j)) * texel).rgb;
            mean[2] += c; count[2] += 1.0;
        }
        for (int i = 0; i <= r; i++) {
            vec3 c = texture(uTexture, vTexCoord + vec2(float(i), float(j)) * texel).rgb;
            mean[3] += c; count[3] += 1.0;
        }
    }

    for (int q = 0; q < 4; q++) {
        mean[q] /= count[q];
    }

    // Recompute variance per quadrant
    for (int j = -r; j <= 0; j++) {
        for (int i = -r; i <= 0; i++) {
            vec3 c = texture(uTexture, vTexCoord + vec2(float(i), float(j)) * texel).rgb;
            vec3 d = c - mean[0]; variance[0] += dot(d, d);
        }
        for (int i = 0; i <= r; i++) {
            vec3 c = texture(uTexture, vTexCoord + vec2(float(i), float(j)) * texel).rgb;
            vec3 d = c - mean[1]; variance[1] += dot(d, d);
        }
    }
    for (int j = 0; j <= r; j++) {
        for (int i = -r; i <= 0; i++) {
            vec3 c = texture(uTexture, vTexCoord + vec2(float(i), float(j)) * texel).rgb;
            vec3 d = c - mean[2]; variance[2] += dot(d, d);
        }
        for (int i = 0; i <= r; i++) {
            vec3 c = texture(uTexture, vTexCoord + vec2(float(i), float(j)) * texel).rgb;
            vec3 d = c - mean[3]; variance[3] += dot(d, d);
        }
    }

    // Pick quadrant with smallest variance (smoothest)
    int best = 0;
    float minVar = variance[0];
    for (int q = 1; q < 4; q++) {
        if (variance[q] < minVar) { minVar = variance[q]; best = q; }
    }

    float alpha = texture(uTexture, vTexCoord).a;
    fragColor = vec4(mean[best], alpha);
}
)";

// ===========================================================================
// ShaderEffectInstance
// ===========================================================================

ShaderEffectInstance::ShaderEffectInstance(const ShaderEffectDef &def, QObject *parent)
    : QObject(parent), m_def(def)
{
    resetToDefaults();
}

void ShaderEffectInstance::resetToDefaults()
{
    m_values.clear();
    for (const ParamDef &p : m_def.params) {
        m_values[p.name] = p.defaultVal;
    }
}

void ShaderEffectInstance::setParam(const QString &name, const QVariant &value)
{
    m_values[name] = value;
    emit paramChanged(name, value);
}

QVariant ShaderEffectInstance::getParam(const QString &name) const
{
    return m_values.value(name);
}

// ===========================================================================
// ShaderEffectLibrary
// ===========================================================================

ShaderEffectLibrary &ShaderEffectLibrary::instance()
{
    static ShaderEffectLibrary lib;
    return lib;
}

ShaderEffectLibrary::ShaderEffectLibrary(QObject *parent)
    : QObject(parent)
{
    registerBuiltins();
}

void ShaderEffectLibrary::registerBuiltins()
{
    // -----------------------------------------------------------------------
    // Color
    // -----------------------------------------------------------------------

    {
        ShaderEffectDef d;
        d.name        = "Chromatic Aberration";
        d.category    = "Color";
        d.description = "RGB channel offset simulating lens chromatic aberration";
        d.fragmentShaderSource = kFragChromaticAberration;
        d.params = {
            {"uAmount", ParamType::Float, 0.0f, 0.05f, 0.01f}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Color Halftone";
        d.category    = "Color";
        d.description = "CMYK halftone dot pattern";
        d.fragmentShaderSource = kFragColorHalftone;
        d.params = {
            {"uDotSize",    ParamType::Float, 4.0f,   20.0f,   8.0f},
            {"uResolution", ParamType::Vec2,  QVariant(), QVariant(), QVariant::fromValue(QVector2D(1920, 1080))}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Duotone";
        d.category    = "Color";
        d.description = "Map luminance to two user-defined colors";
        d.fragmentShaderSource = kFragDuotone;
        d.params = {
            {"uColorShadow",    ParamType::Color, QVariant(), QVariant(), QVariant::fromValue(QVector3D(0.1f, 0.0f, 0.2f))},
            {"uColorHighlight", ParamType::Color, QVariant(), QVariant(), QVariant::fromValue(QVector3D(1.0f, 0.9f, 0.2f))}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Gradient Map";
        d.category    = "Color";
        d.description = "Remap luminance through a 5-stop color gradient";
        d.fragmentShaderSource = kFragGradientMap;
        d.params = {
            {"uStop0", ParamType::Color, QVariant(), QVariant(), QVariant::fromValue(QVector3D(0.0f, 0.0f, 0.0f))},
            {"uStop1", ParamType::Color, QVariant(), QVariant(), QVariant::fromValue(QVector3D(0.2f, 0.0f, 0.5f))},
            {"uStop2", ParamType::Color, QVariant(), QVariant(), QVariant::fromValue(QVector3D(0.8f, 0.1f, 0.3f))},
            {"uStop3", ParamType::Color, QVariant(), QVariant(), QVariant::fromValue(QVector3D(1.0f, 0.6f, 0.0f))},
            {"uStop4", ParamType::Color, QVariant(), QVariant(), QVariant::fromValue(QVector3D(1.0f, 1.0f, 0.9f))}
        };
        m_effects.append(d);
    }

    // -----------------------------------------------------------------------
    // Blur
    // -----------------------------------------------------------------------

    {
        ShaderEffectDef d;
        d.name        = "Gaussian Blur";
        d.category    = "Blur";
        d.description = "Separable Gaussian blur (horizontal + vertical pass)";
        // The renderer handles two-pass automatically for this effect
        d.fragmentShaderSource = kFragGaussianBlurH;  // H pass stored here
        d.params = {
            {"uRadius",     ParamType::Float, 1.0f,  30.0f, 5.0f},
            {"uResolution", ParamType::Vec2,  QVariant(), QVariant(), QVariant::fromValue(QVector2D(1920, 1080))}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Radial Blur";
        d.category    = "Blur";
        d.description = "Zoom blur radiating from a center point";
        d.fragmentShaderSource = kFragRadialBlur;
        d.params = {
            {"uStrength", ParamType::Float, 0.0f, 0.1f,  0.02f},
            {"uCenter",   ParamType::Vec2,  QVariant(), QVariant(), QVariant::fromValue(QVector2D(0.5f, 0.5f))},
            {"uSamples",  ParamType::Int,   8,    32,    16}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Directional Blur";
        d.category    = "Blur";
        d.description = "Motion blur in a specified direction";
        d.fragmentShaderSource = kFragDirectionalBlur;
        d.params = {
            {"uAngle",      ParamType::Float, 0.0f,   360.0f, 0.0f},
            {"uStrength",   ParamType::Float, 0.0f,   0.05f,  0.01f},
            {"uSamples",    ParamType::Int,   8,      32,     16},
            {"uResolution", ParamType::Vec2,  QVariant(), QVariant(), QVariant::fromValue(QVector2D(1920, 1080))}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Tilt Shift";
        d.category    = "Blur";
        d.description = "Selective focus miniature/tilt-shift look";
        d.fragmentShaderSource = kFragTiltShift;
        d.params = {
            {"uFocusCenter", ParamType::Float, 0.0f,  1.0f,   0.5f},
            {"uFocusRange",  ParamType::Float, 0.0f,  0.5f,   0.1f},
            {"uBlurRadius",  ParamType::Float, 1.0f,  20.0f,  8.0f},
            {"uResolution",  ParamType::Vec2,  QVariant(), QVariant(), QVariant::fromValue(QVector2D(1920, 1080))}
        };
        m_effects.append(d);
    }

    // -----------------------------------------------------------------------
    // Distort
    // -----------------------------------------------------------------------

    {
        ShaderEffectDef d;
        d.name        = "Barrel Distortion";
        d.category    = "Distort";
        d.description = "Barrel or pincushion lens distortion correction";
        d.fragmentShaderSource = kFragBarrelDistortion;
        d.params = {
            {"uK1", ParamType::Float, -0.5f, 0.5f,  -0.2f},
            {"uK2", ParamType::Float, -0.2f, 0.2f,   0.05f}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Ripple";
        d.category    = "Distort";
        d.description = "Animated water ripple distortion";
        d.fragmentShaderSource = kFragRipple;
        d.params = {
            {"uTime",      ParamType::Float, 0.0f,  1000.0f, 0.0f},
            {"uAmplitude", ParamType::Float, 0.0f,  0.05f,   0.01f},
            {"uFrequency", ParamType::Float, 5.0f,  30.0f,   12.0f},
            {"uSpeed",     ParamType::Float, 0.5f,  5.0f,    2.0f}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Pixelate";
        d.category    = "Distort";
        d.description = "Block pixelation effect";
        d.fragmentShaderSource = kFragPixelate;
        d.params = {
            {"uBlockSize",  ParamType::Float, 2.0f, 64.0f,  8.0f},
            {"uResolution", ParamType::Vec2,  QVariant(), QVariant(), QVariant::fromValue(QVector2D(1920, 1080))}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Glitch";
        d.category    = "Distort";
        d.description = "Digital glitch with RGB split, block shift and scanlines";
        d.fragmentShaderSource = kFragGlitch;
        d.params = {
            {"uTime",       ParamType::Float, 0.0f,  1000.0f, 0.0f},
            {"uIntensity",  ParamType::Float, 0.0f,  1.0f,    0.3f},
            {"uScanlines",  ParamType::Float, 0.0f,  1.0f,    0.4f},
            {"uResolution", ParamType::Vec2,  QVariant(), QVariant(), QVariant::fromValue(QVector2D(1920, 1080))}
        };
        m_effects.append(d);
    }

    // -----------------------------------------------------------------------
    // Stylize
    // -----------------------------------------------------------------------

    {
        ShaderEffectDef d;
        d.name        = "Film Grain";
        d.category    = "Stylize";
        d.description = "Procedural animated film grain noise";
        d.fragmentShaderSource = kFragFilmGrain;
        d.params = {
            {"uAmount", ParamType::Float, 0.0f,  0.5f,    0.08f},
            {"uTime",   ParamType::Float, 0.0f,  1000.0f, 0.0f}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Vignette";
        d.category    = "Stylize";
        d.description = "Darkened edge vignette";
        d.fragmentShaderSource = kFragVignette;
        d.params = {
            {"uIntensity", ParamType::Float, 0.0f, 1.0f, 0.5f},
            {"uRadius",    ParamType::Float, 0.3f, 1.0f, 0.7f},
            {"uSoftness",  ParamType::Float, 0.1f, 0.8f, 0.4f}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "CRT / Retro";
        d.category    = "Stylize";
        d.description = "CRT monitor look: scanlines, barrel curvature, bloom and phosphor tint";
        d.fragmentShaderSource = kFragCRT;
        d.params = {
            {"uScanlineIntensity", ParamType::Float, 0.0f, 1.0f,   0.4f},
            {"uCurvature",         ParamType::Float, 0.0f, 0.5f,   0.08f},
            {"uBloom",             ParamType::Float, 0.0f, 0.5f,   0.1f},
            {"uResolution",        ParamType::Vec2,  QVariant(), QVariant(), QVariant::fromValue(QVector2D(1920, 1080))}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Sketch / Pencil";
        d.category    = "Stylize";
        d.description = "Sobel edge detection rendered as pencil sketch on white paper";
        d.fragmentShaderSource = kFragSketch;
        d.params = {
            {"uEdgeStrength",  ParamType::Float, 0.5f, 5.0f,  2.0f},
            {"uLineThickness", ParamType::Float, 0.5f, 3.0f,  1.0f},
            {"uResolution",    ParamType::Vec2,  QVariant(), QVariant(), QVariant::fromValue(QVector2D(1920, 1080))}
        };
        m_effects.append(d);
    }

    {
        ShaderEffectDef d;
        d.name        = "Oil Paint";
        d.category    = "Stylize";
        d.description = "Kuwahara filter oil-painting approximation";
        d.fragmentShaderSource = kFragOilPaint;
        d.params = {
            {"uRadius",     ParamType::Int,  2,  6,   4},
            {"uResolution", ParamType::Vec2, QVariant(), QVariant(), QVariant::fromValue(QVector2D(1920, 1080))}
        };
        m_effects.append(d);
    }
}

QVector<ShaderEffectDef> ShaderEffectLibrary::allEffects() const
{
    return m_effects;
}

QVector<ShaderEffectDef> ShaderEffectLibrary::effectsByCategory(const QString &category) const
{
    QVector<ShaderEffectDef> result;
    for (const ShaderEffectDef &d : m_effects) {
        if (d.category == category)
            result.append(d);
    }
    return result;
}

QStringList ShaderEffectLibrary::categories() const
{
    QStringList cats;
    for (const ShaderEffectDef &d : m_effects) {
        if (!cats.contains(d.category))
            cats.append(d.category);
    }
    return cats;
}

ShaderEffectDef ShaderEffectLibrary::findByName(const QString &name) const
{
    for (const ShaderEffectDef &d : m_effects) {
        if (d.name == name)
            return d;
    }
    return ShaderEffectDef{};  // empty — check .name.isEmpty()
}

// ===========================================================================
// ShaderEffectRenderer
// ===========================================================================

ShaderEffectRenderer::ShaderEffectRenderer(QObject *parent)
    : QObject(parent), QOpenGLFunctions(), m_vbo(QOpenGLBuffer::VertexBuffer)
{
}

ShaderEffectRenderer::~ShaderEffectRenderer()
{
    qDeleteAll(m_programs);
    m_programs.clear();

    delete m_fbo[0];
    delete m_fbo[1];

    m_vbo.destroy();
    m_vao.destroy();
}

void ShaderEffectRenderer::initialize()
{
    if (m_initialized) return;
    initializeOpenGLFunctions();
    setupQuad();
    m_initialized = true;
}

void ShaderEffectRenderer::setupQuad()
{
    // Same layout as GLPreview: pos(xy) + texcoord(uv)
    static const float kQuadVerts[] = {
        -1.0f,  1.0f,  0.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
    };

    m_vao.create();
    m_vao.bind();

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(kQuadVerts, sizeof(kQuadVerts));

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void *>(2 * sizeof(float)));

    m_vbo.release();
    m_vao.release();
}

bool ShaderEffectRenderer::compile(const ShaderEffectDef &def)
{
    if (m_programs.contains(def.name))
        return true;  // already compiled

    auto *prog = new QOpenGLShaderProgram();
    if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, s_vertexShaderSrc)) {
        m_lastError = QString("Vertex shader error [%1]: %2").arg(def.name, prog->log());
        delete prog;
        return false;
    }
    if (!prog->addShaderFromSourceCode(QOpenGLShader::Fragment, def.fragmentShaderSource)) {
        m_lastError = QString("Fragment shader error [%1]: %2").arg(def.name, prog->log());
        delete prog;
        return false;
    }
    if (!prog->link()) {
        m_lastError = QString("Shader link error [%1]: %2").arg(def.name, prog->log());
        delete prog;
        return false;
    }

    m_programs.insert(def.name, prog);
    return true;
}

void ShaderEffectRenderer::ensureFBOs(int width, int height)
{
    if (m_fbo[0] && m_fboWidth == width && m_fboHeight == height)
        return;

    delete m_fbo[0]; m_fbo[0] = nullptr;
    delete m_fbo[1]; m_fbo[1] = nullptr;

    QOpenGLFramebufferObjectFormat fmt;
    fmt.setInternalTextureFormat(GL_RGBA8);
    fmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);

    m_fbo[0] = new QOpenGLFramebufferObject(width, height, fmt);
    m_fbo[1] = new QOpenGLFramebufferObject(width, height, fmt);
    m_fboWidth  = width;
    m_fboHeight = height;
}

void ShaderEffectRenderer::bindUniforms(QOpenGLShaderProgram *prog,
                                        const ShaderEffectInstance &instance)
{
    prog->setUniformValue("uTexture", 0);

    for (const ParamDef &p : instance.def().params) {
        QVariant val = instance.getParam(p.name);
        if (!val.isValid()) continue;

        QByteArray  nameBa = p.name.toLatin1();
        const char *nameC  = nameBa.constData();

        switch (p.type) {
        case ParamType::Float:
            prog->setUniformValue(nameC, val.toFloat());
            break;
        case ParamType::Int:
            prog->setUniformValue(nameC, val.toInt());
            break;
        case ParamType::Bool:
            prog->setUniformValue(nameC, val.toBool());
            break;
        case ParamType::Vec2:
        case ParamType::Vec3:
        case ParamType::Color: {
            QVector3D v3 = val.value<QVector3D>();
            QVector2D v2 = val.value<QVector2D>();
            if (p.type == ParamType::Vec2) {
                prog->setUniformValue(nameC, v2);
            } else {
                prog->setUniformValue(nameC, v3);
            }
            break;
        }
        }
    }
}

void ShaderEffectRenderer::drawQuad()
{
    m_vao.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();
}

void ShaderEffectRenderer::render(GLuint inputTextureId,
                                  const ShaderEffectInstance &instance,
                                  QOpenGLFramebufferObject *outputFBO,
                                  int width, int height)
{
    const QString &name = instance.def().name;

    if (!compile(instance.def())) {
        qWarning() << "ShaderEffectRenderer::render — compile failed:" << m_lastError;
        return;
    }

    QOpenGLShaderProgram *prog = m_programs.value(name);
    if (!prog) return;

    // For Gaussian Blur we need a two-pass approach
    if (name == "Gaussian Blur") {
        ensureFBOs(width, height);

        // --- Horizontal pass: input → fbo[0] ---
        // Compile the V-pass shader under a temporary name
        const QString vPassName = name + "__VPass";
        if (!m_programs.contains(vPassName)) {
            auto *vprog = new QOpenGLShaderProgram();
            vprog->addShaderFromSourceCode(QOpenGLShader::Vertex, s_vertexShaderSrc);
            vprog->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragGaussianBlurV);
            vprog->link();
            m_programs.insert(vPassName, vprog);
        }
        QOpenGLShaderProgram *hprog = prog;
        QOpenGLShaderProgram *vprog = m_programs.value(vPassName);

        // H pass
        m_fbo[0]->bind();
        glViewport(0, 0, width, height);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, inputTextureId);
        hprog->bind();
        bindUniforms(hprog, instance);
        drawQuad();
        hprog->release();
        m_fbo[0]->release();

        // V pass: fbo[0] → outputFBO
        if (outputFBO) outputFBO->bind();
        glViewport(0, 0, width, height);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_fbo[0]->texture());
        vprog->bind();
        bindUniforms(vprog, instance);
        drawQuad();
        vprog->release();
        if (outputFBO) outputFBO->release();

        return;
    }

    // Single-pass rendering
    if (outputFBO) outputFBO->bind();
    glViewport(0, 0, width, height);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inputTextureId);

    prog->bind();
    bindUniforms(prog, instance);
    drawQuad();
    prog->release();

    glBindTexture(GL_TEXTURE_2D, 0);
    if (outputFBO) outputFBO->release();
}

GLuint ShaderEffectRenderer::renderChain(GLuint inputTextureId,
                                         const QVector<ShaderEffectInstance *> &chain,
                                         int width, int height)
{
    if (chain.isEmpty())
        return inputTextureId;

    ensureFBOs(width, height);

    int ping = 0;
    int pong = 1;
    GLuint srcTex = inputTextureId;

    for (int i = 0; i < chain.size(); ++i) {
        ShaderEffectInstance *inst = chain[i];
        if (!inst) continue;

        // Last effect writes to pong; intermediate effects alternate
        QOpenGLFramebufferObject *dstFBO = m_fbo[pong];

        render(srcTex, *inst, dstFBO, width, height);

        srcTex = dstFBO->texture();
        std::swap(ping, pong);
    }

    // srcTex now holds the result of the last render
    return srcTex;
}
