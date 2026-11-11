// US-2200: GL preview 3D LUT read-back against the CPU LutImporter::applyLut.
//
// The widget is 16:9 (256x144) because GLPreview always draws the 16:9 canvas
// frame overlay: at this size it lies on the widget perimeter only, so columns
// 0 / 255 and rows 0 / 143 are left out of every comparison. The frame is four 36-row bands:
// band 0 = grey ramp, band 1 = red-only ramp, band 2 = (26,26,26), band 3 =
// left half (64,64,64) / right half (191,191,191). No timeline, so no grade.
//
// G1 half texel: a 2-point identity maps 0.25 -> 64 and 0.75 -> 191 (HEAD
//    sampled the knots at the texture edge: 1 / 254). Precondition: after
//    clearLut the read-back equals the one with no LUT.
// G2 narrow domain: LUT_3D_SIZE 3, DOMAIN 0.1..0.101, rows 0 / 0.25 / 1 maps
//    (26,26,26) -> 255 like LutImporter::sample; control (13,13,13) -> 0
//    (HEAD resampled the domain onto a 65-point grid: 33).
// G3 sweep: 33-point identity, 17-point v^2 and 65-point identity on bands 0
//    and 1 match applyLut within 2/255 (HEAD 5 / 15 / 3).
// US-2201: a 1D LUT is an N x 1 RGB32F texture sampled per channel at texel centres.
// G4 1D sine: 1024 points, 8 cycles, amplitude 0.45, default domain; bands 0 and 1 match
//    applyLut within 2/255 (HEAD's 65-point grid erred by 44). Band 1 is a red-only ramp, so a
//    lookup that used the red coordinate for every channel fails it.
// G5 1D domain: LUT_1D_SIZE 3 rows 0 / 0.25 / 1, DOMAIN 0.1..0.6: (26) follows sample(),
//    (153) -> 255, (255) -> 255 (HEAD's 3^3 grid gave 219 for 153).
// G6 1D resample: 65536 points with setLut1DMaxTextureWidthForTest(1024) uploads a 1024-wide
//    texture whose bands 0 and 1 match applyLut within 2/255. The context's real
//    GL_MAX_TEXTURE_SIZE shows as the width of an unforced upload (INFO line, not a gate).
// G7 fresh 1D-first: a new GLPreview that never held a 3D LUT shows a 1D LUT. No GL error
//    after the draw, pixels match applyLut within kTol, and uLut3D's unit (glGetUniformiv)
//    differs from every sampler2D's unit, since lenient drivers draw despite the clash.
// GL unavailable (invalid context or null grab): G1-G7 are skipped.
#include "SelftestGate.h"
#include "../GLPreview.h"
#include "../LutImporter.h"
#include "../VideoPlayer.h"

#include <QApplication>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>

namespace {

constexpr int kW = 256;
constexpr int kH = 144;
constexpr int kBand = 36;
constexpr int kTol = 2;

QImage bandInput()
{
    QImage im(kW, kH, QImage::Format_RGB32);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            QRgb c = 0;
            switch (y / kBand) {
            case 0: c = qRgb(x, x, x); break;
            case 1: c = qRgb(x, 0, 0); break;
            case 2: c = qRgb(26, 26, 26); break;
            default: c = x < kW / 2 ? qRgb(64, 64, 64) : qRgb(191, 191, 191); break;
            }
            im.setPixel(x, y, c);
        }
    return im;
}

LutData make3D(int n, const std::function<float(float)> &f)
{
    LutData l;
    l.type = LutType::Cube3D;
    l.size = n;
    l.name = QStringLiteral("lut-gl");
    l.table.resize(qint64(n) * n * n);
    int i = 0;
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
                l.table[i++] = QVector3D(f(r / float(n - 1)), f(g / float(n - 1)), f(b / float(n - 1)));
    return l;
}

LutData make1D(int n, const std::function<float(float)> &f)
{
    LutData l;
    l.type = LutType::Cube1D;
    l.size = n;
    l.name = QStringLiteral("lut-gl-1d");
    l.table.resize(n);
    for (int i = 0; i < n; ++i) {
        const float v = f(i / float(n - 1));
        l.table[i] = QVector3D(v, v, v);
    }
    return l;
}

// The canvas frame overlay covers the widget perimeter.
bool onFrame(int x, int y) { return x == 0 || x == kW - 1 || y == 0 || y == kH - 1; }

// Largest per-channel |a - b| over bands [b0, b1] off the frame (columns 1 .. kW - 2, rows 1 .. kH - 2).
struct Diff {
    int maxErr = 255;
    int x = -1, y = -1, ch = -1, a = -1, b = -1;
    std::string str() const
    {
        return "maxErr=" + std::to_string(maxErr) + "@(x=" + std::to_string(x) + ",y=" + std::to_string(y)
             + ",ch=" + std::to_string(ch) + " gl=" + std::to_string(a) + " ref=" + std::to_string(b) + ")";
    }
};

Diff compareBands(const QImage &gl, const QImage &ref0, int b0, int b1)
{
    Diff d;
    if (gl.isNull() || ref0.isNull())
        return d;
    const QImage ref = ref0.convertToFormat(QImage::Format_RGB32);
    d.maxErr = 0;
    for (int y = b0 * kBand; y < (b1 + 1) * kBand; ++y)
        for (int x = 1; x < kW - 1; ++x) {
            if (onFrame(x, y))
                continue;
            const QRgb p = gl.pixel(x, y), q = ref.pixel(x, y);
            const int pv[3] = {qRed(p), qGreen(p), qBlue(p)};
            const int qv[3] = {qRed(q), qGreen(q), qBlue(q)};
            for (int c = 0; c < 3; ++c) {
                const int e = std::abs(pv[c] - qv[c]);
                if (e > d.maxErr || d.x < 0) {
                    d.maxErr = std::max(d.maxErr, e);
                    d.x = x; d.y = y; d.ch = c; d.a = pv[c]; d.b = qv[c];
                }
            }
        }
    return d;
}

QImage filled(int v)
{
    QImage im(kW, kH, QImage::Format_RGB32);
    im.fill(qRgb(v, v, v));
    return im;
}

} // namespace

int runLutGlSelftest()
{
    selftest::GateTally tally;

    VideoPlayer player;
    GLPreview *gl = player.glPreview();
    bool glOk = false;
    if (gl) {
        gl->setFixedSize(kW, kH);
        gl->setFrameTextureNearestForTest(true);
        player.show();
        for (int i = 0; i < 8 && !glOk; ++i) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            const QImage probe = gl->grabFramebuffer();
            glOk = gl->context() && gl->context()->isValid() && !probe.isNull();
        }
    }
    if (!glOk) {
        for (int g = 1; g <= 7; ++g)
            tally.skip(g, "no usable QOpenGLWidget context");
        return tally.finish("lut-gl");
    }

    // grabFramebuffer returns device pixels (dpr x 256x144). The frame texture is
    // sampled Nearest, so frame pixel (x, y) is device pixel
    // (floor((x + 0.5) * dpr), floor((y + 0.5) * dpr)); a grab of another size is null.
    bool sizeOk = true;
    const auto grabFrom = [&](GLPreview *w) -> QImage {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        const QImage raw = w->grabFramebuffer();
        const qreal dpr = w->devicePixelRatioF();
        const QSize device(qRound(kW * dpr), qRound(kH * dpr));
        if (raw.isNull() || raw.size() != device) {
            sizeOk = false;
            std::cerr << "lut-gl grab size mismatch raw=" << raw.width() << 'x' << raw.height()
                      << " expectedDevice=" << device.width() << 'x' << device.height() << " dpr=" << dpr << '\n';
            return QImage();
        }
        QImage frame(kW, kH, QImage::Format_RGB32);
        for (int y = 0; y < kH; ++y) {
            const int dy = std::min(raw.height() - 1, int(std::floor((y + 0.5) * dpr)));
            for (int x = 0; x < kW; ++x) {
                const int dx = std::min(raw.width() - 1, int(std::floor((x + 0.5) * dpr)));
                frame.setPixel(x, y, raw.pixel(dx, dy) | 0xff000000u);
            }
        }
        return frame;
    };
    const auto grab = [&]() { return grabFrom(gl); };
    const auto render = [&](const QImage &in, const LutData *lut) {
        gl->displayFrame(in);
        if (lut)
            gl->setLut(*lut);
        gl->displayFrame(in);
        return grab();
    };

    const QImage in = bandInput();
    const auto id = [](float x) { return x; };

    // G1: 2-point identity; precondition clearLut == no LUT.
    {
        const QImage none = render(in, nullptr);
        const Diff noneVsIn = compareBands(none, in, 0, 3);
        const LutData two = make3D(2, id);
        const QImage g = render(in, &two);
        Diff lo, hi; // band 3: left half 64 (0.25), right half 191 (0.75)
        lo.maxErr = hi.maxErr = g.isNull() ? 255 : 0;
        if (!g.isNull())
            for (int y = 3 * kBand; y < 4 * kBand; ++y)
                for (int x = 1; x < kW - 1; ++x) {
                    if (onFrame(x, y))
                        continue;
                    const int want = x < kW / 2 ? 64 : 191;
                    const QRgb p = g.pixel(x, y);
                    Diff &d = x < kW / 2 ? lo : hi;
                    for (int v : {qRed(p), qGreen(p), qBlue(p)})
                        if (std::abs(v - want) > d.maxErr || d.x < 0) {
                            d.maxErr = std::max(d.maxErr, std::abs(v - want));
                            d.x = x; d.y = y; d.a = v; d.b = want; d.ch = 0;
                        }
                }
        const Diff vsRef = compareBands(g, LutImporter::applyLut(in, two), 0, 3);
        gl->clearLut();
        const QImage cleared = render(in, nullptr);
        const Diff clearedVsNone = compareBands(cleared, none, 0, 3);
        const bool ok = sizeOk && noneVsIn.maxErr <= kTol && clearedVsNone.maxErr == 0
            && lo.maxErr <= kTol && hi.maxErr <= kTol;
        tally.gate(1, ok, "noLut_vs_input " + noneVsIn.str() + " | cleared_vs_noLut " + clearedVsNone.str()
                   + " | 0.25->64 " + lo.str() + " | 0.75->191 " + hi.str() + " | 2pt_vs_applyLut " + vsRef.str()
                   + " sizeOk=" + std::to_string(sizeOk));
    }

    // G2: narrow domain 0.1..0.101.
    {
        LutData nd = make3D(3, [](float x) { return x < 0.25f ? 0.0f : (x < 0.75f ? 0.25f : 1.0f); });
        nd.domainMin = QVector3D(0.1f, 0.1f, 0.1f);
        nd.domainMax = QVector3D(0.101f, 0.101f, 0.101f);
        const float t26 = 26.0f / 255.0f;
        const QVector3D s26 = LutImporter::sample(nd, QVector3D(t26, t26, t26));
        const QImage g = render(in, &nd);
        const Diff d26 = compareBands(g, filled(255), 2, 2);
        gl->clearLut();
        const QImage g13 = render(filled(13), &nd);
        const Diff d13 = compareBands(g13, filled(0), 0, 3);
        gl->clearLut();
        const bool sampleOk = std::abs(s26.x() - 1.0f) < 1e-6f && std::abs(s26.y() - 1.0f) < 1e-6f
            && std::abs(s26.z() - 1.0f) < 1e-6f;
        const bool ok = sizeOk && sampleOk && d26.maxErr <= kTol && d13.maxErr <= kTol;
        tally.gate(2, ok, "sample26=" + std::to_string(s26.x()) + " | (26)->255 " + d26.str()
                   + " | control (13)->0 " + d13.str() + " sizeOk=" + std::to_string(sizeOk));
    }

    // G3: sweeps on bands 0 / 1 against applyLut.
    {
        struct Leg { const char *tag; LutData lut; };
        const Leg legs[3] = {{"(a) 33pt identity", make3D(33, id)},
                             {"(b) 17pt v^2", make3D(17, [](float x) { return x * x; })},
                             {"(c) 65pt identity", make3D(65, id)}};
        bool ok = sizeOk;
        std::string detail;
        for (const Leg &leg : legs) {
            const QImage g = render(in, &leg.lut);
            const Diff d = compareBands(g, LutImporter::applyLut(in, leg.lut), 0, 1);
            gl->clearLut();
            ok = ok && d.maxErr <= kTol;
            detail += std::string(detail.empty() ? "" : " | ") + leg.tag + " " + d.str();
        }
        tally.gate(3, ok, detail + " sizeOk=" + std::to_string(sizeOk));
    }

    // G4: 1024-point 8-cycle sine on bands 0 / 1 against applyLut.
    {
        const float kTwoPi = 6.28318530718f;
        const LutData sine = make1D(1024, [kTwoPi](float x) { return 0.5f + 0.45f * std::sin(kTwoPi * 8.0f * x); });
        const QImage g = render(in, &sine);
        const Diff d = compareBands(g, LutImporter::applyLut(in, sine), 0, 1);
        const int w = gl->lut1DTextureWidthForTest();
        gl->clearLut();
        tally.gate(4, sizeOk && w == 1024 && d.maxErr <= kTol,
                   "1024pt 8-cycle sine bands0-1 " + d.str() + " width=" + std::to_string(w)
                   + " sizeOk=" + std::to_string(sizeOk));
    }

    // G5: 1D LUT with DOMAIN 0.1..0.6.
    {
        LutData dom = make1D(3, [](float x) { return x < 0.25f ? 0.0f : (x < 0.75f ? 0.25f : 1.0f); });
        dom.domainMin = QVector3D(0.1f, 0.1f, 0.1f);
        dom.domainMax = QVector3D(0.6f, 0.6f, 0.6f);
        const float t26 = 26.0f / 255.0f;
        const int want26 = qRound(LutImporter::sample(dom, QVector3D(t26, t26, t26)).x() * 255.0f);
        const QImage g26 = render(filled(26), &dom);
        const Diff d26 = compareBands(g26, filled(want26), 0, 3);
        const QImage g153 = render(filled(153), &dom);
        const Diff d153 = compareBands(g153, filled(255), 0, 3);
        const QImage g255 = render(filled(255), &dom);
        const Diff d255 = compareBands(g255, filled(255), 0, 3);
        gl->clearLut();
        tally.gate(5, sizeOk && d26.maxErr <= kTol && d153.maxErr <= kTol && d255.maxErr <= kTol,
                   "(26)->" + std::to_string(want26) + " " + d26.str() + " | (153)->255 " + d153.str()
                   + " | (255)->255 " + d255.str() + " sizeOk=" + std::to_string(sizeOk));
    }

    // G6: 65536 points forced through a 1024-wide texture.
    {
        const float kTwoPi = 6.28318530718f;
        const LutData big = make1D(65536, [kTwoPi](float x) { return 0.5f + 0.5f * std::sin(kTwoPi * x); });
        gl->setLut1DMaxTextureWidthForTest(1024);
        const QImage g = render(in, &big);
        const int w = gl->lut1DTextureWidthForTest();
        const Diff d = compareBands(g, LutImporter::applyLut(in, big), 0, 1);
        gl->setLut1DMaxTextureWidthForTest(0);
        gl->setLut(big);
        std::cout << "lut-gl INFO: 65536-point 1D LUT without override uploads a texture of width "
                  << gl->lut1DTextureWidthForTest() << " (min(65536, GL_MAX_TEXTURE_SIZE))\n";
        gl->clearLut();
        tally.gate(6, sizeOk && w == 1024 && d.maxErr <= kTol,
                   "65536pt -> width=" + std::to_string(w) + " bands0-1 " + d.str()
                   + " sizeOk=" + std::to_string(sizeOk));
    }

    // G7: a fresh GLPreview that never held a 3D LUT shows a 1D LUT first. uLut3D must not
    // share unit 0 with uTexture (GL_INVALID_OPERATION, no frame); error drained before the draw.
    {
        VideoPlayer fresh;
        GLPreview *fg = fresh.glPreview();
        bool fOk = false;
        if (fg) {
            fg->setFixedSize(kW, kH);
            fg->setFrameTextureNearestForTest(true);
            fresh.show();
            for (int i = 0; i < 8 && !fOk; ++i) {
                QApplication::processEvents(QEventLoop::AllEvents, 25);
                const QImage probe = fg->grabFramebuffer();
                fOk = fg->context() && fg->context()->isValid() && !probe.isNull();
            }
        }
        if (!fOk) {
            tally.skip(7, "no usable QOpenGLWidget context for the fresh preview");
        } else {
            const float kTwoPi = 6.28318530718f;
            const LutData sine = make1D(1024, [kTwoPi](float x) { return 0.5f + 0.45f * std::sin(kTwoPi * 8.0f * x); });
            fg->displayFrame(in);
            fg->setLut(sine);
            fg->displayFrame(in);
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            fg->makeCurrent();
            while (fg->context()->functions()->glGetError() != GL_NO_ERROR) {}
            fg->doneCurrent();
            const QImage g = grabFrom(fg);
            fg->makeCurrent();
            const GLenum err = fg->context()->functions()->glGetError();
            fg->doneCurrent();
            const QImage ref = LutImporter::applyLut(in, sine);
            const Diff d = compareBands(g, ref, 0, 1);
            const int px = 100, py = 10; // band 0 grey ramp: input 100
            const int gotPx = g.isNull() ? -1 : qRed(g.pixel(px, py));
            const int wantPx = qRed(ref.convertToFormat(QImage::Format_RGB32).pixel(px, py));
            // Driver-independent half: a lenient driver draws despite the clash, so read the
            // sampler3D's unit back and require it off every sampler2D's unit.
            const int u3 = fg->samplerUnitForTest("uLut3D");
            const int uTex = fg->samplerUnitForTest("uTexture");
            const int u1 = fg->samplerUnitForTest("uLut1D");
            const int uCurve = fg->samplerUnitForTest("uCurveLut");
            const int uHue = fg->samplerUnitForTest("uHueVsSatLut");
            const bool unitsOk = u3 >= 0 && u3 != uTex && u3 != u1 && u3 != uCurve && u3 != uHue;
            const bool ok = sizeOk && err == GL_NO_ERROR && !g.isNull() && std::abs(gotPx - wantPx) <= kTol
                && d.maxErr <= kTol && unitsOk;
            tally.gate(7, ok, "fresh preview 1D-first glError=0x" + [&] { char b[16]; snprintf(b, sizeof b, "%x", unsigned(err)); return std::string(b); }()
                       + " pixel(100,10) measured=" + std::to_string(gotPx) + " expected=" + std::to_string(wantPx)
                       + " | bands0-1 " + d.str() + " sizeOk=" + std::to_string(sizeOk)
                       + " | units uLut3D=" + std::to_string(u3) + " uTexture=" + std::to_string(uTex)
                       + " uLut1D=" + std::to_string(u1) + " uCurveLut=" + std::to_string(uCurve)
                       + " uHueVsSatLut=" + std::to_string(uHue));
        }
    }

    return tally.finish("lut-gl");
}
