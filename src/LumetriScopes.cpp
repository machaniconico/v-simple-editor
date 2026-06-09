#include "LumetriScopes.h"

#include <QPainter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QtMath>

namespace {

// Decimate the source frame to a fixed pixel budget so scope math stays
// O(constant) regardless of preview size. ~10k samples produces stable
// histograms at <1 ms cost on a single thread.
constexpr int kSampleBudget = 10000;

inline QImage downsampleToBudget(const QImage &src) {
    const int srcArea = src.width() * src.height();
    if (srcArea <= kSampleBudget) return src;
    const double scale = std::sqrt(static_cast<double>(kSampleBudget) /
                                   static_cast<double>(srcArea));
    const int w = qMax(16, static_cast<int>(src.width()  * scale));
    const int h = qMax(16, static_cast<int>(src.height() * scale));
    return src.scaled(w, h, Qt::IgnoreAspectRatio, Qt::FastTransformation)
              .convertToFormat(QImage::Format_RGB32);
}

// Vectorscope-specific downsample: cap at 64x64 grid (= 4096 samples) for
// bounded CPU. The polar accumulation runs on every repaint but costs O(n)
// in decoded pixels; keeping n ≤ 4096 keeps < 1 ms on a single thread.
inline QImage downsampleForVector(const QImage &src) {
    constexpr int kMaxGrid = 64 * 64;
    const int srcArea = src.width() * src.height();
    if (srcArea <= kMaxGrid)
        return src.convertToFormat(QImage::Format_RGB32);
    const double scale = std::sqrt(static_cast<double>(kMaxGrid) /
                                   static_cast<double>(srcArea));
    const int w = qMax(4, static_cast<int>(src.width()  * scale));
    const int h = qMax(4, static_cast<int>(src.height() * scale));
    return src.scaled(w, h, Qt::IgnoreAspectRatio, Qt::FastTransformation)
              .convertToFormat(QImage::Format_RGB32);
}

} // namespace

RgbParadeData computeRgbParadeData(const QImage &frame)
{
    RgbParadeData data;
    const QImage src = downsampleToBudget(frame).convertToFormat(QImage::Format_RGB32);
    if (src.isNull() || src.width() <= 0 || src.height() <= 0)
        return data;

    data.sourceHeight = src.height();
    for (auto &channel : data.channels) {
        channel.resize(src.width());
        for (auto &bins : channel)
            bins.fill(0);
    }

    for (int y = 0; y < src.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const QRgb px = line[x];
            ++data.channels[0][x][qRed(px)];
            ++data.channels[1][x][qGreen(px)];
            ++data.channels[2][x][qBlue(px)];
        }
    }

    return data;
}

namespace {

void paintRgbParadeData(QPainter &p, const QRect &target, const RgbParadeData &data)
{
    p.fillRect(target, QColor(20, 20, 22));

    const int W = target.width();
    const int H = target.height();
    if (W <= 0 || H <= 0)
        return;

    const QColor panelBg[3] = {
        QColor(32, 20, 20),
        QColor(20, 30, 22),
        QColor(20, 22, 34),
    };
    const QColor trace[3] = {
        QColor(235, 80, 80, 150),
        QColor(80, 225, 95, 150),
        QColor(95, 135, 245, 150),
    };

    for (int c = 0; c < 3; ++c) {
        const int panelLeft = target.left() + (c * W) / 3;
        const int panelRight = target.left() + ((c + 1) * W) / 3;
        const QRect panel(panelLeft, target.top(), panelRight - panelLeft, H);
        if (panel.width() <= 0)
            continue;

        p.fillRect(panel, panelBg[c]);
        if (c > 0) {
            p.setPen(QPen(QColor(90, 90, 95, 140), 1));
            p.drawLine(panel.left(), panel.top(), panel.left(), panel.bottom());
        }

        if (!data.isEmpty()) {
            const double xs = static_cast<double>(panel.width())
                              / static_cast<double>(data.width());
            const double ys = H > 1 ? static_cast<double>(H - 1) / 255.0 : 0.0;
            p.setPen(QPen(trace[c], 1));
            for (int x = 0; x < data.width(); ++x) {
                const int sx = panel.left()
                               + qMin(panel.width() - 1, static_cast<int>(x * xs));
                const auto &bins = data.channels[c][x];
                for (int intensity = 0; intensity < 256; ++intensity) {
                    const int count = bins[intensity];
                    if (count <= 0)
                        continue;
                    const int sy = target.top() + H - 1
                                   - static_cast<int>(intensity * ys);
                    for (int i = 0; i < count; ++i)
                        p.drawPoint(sx, sy);
                }
            }
        }

        // Reference lines at 0 / 50 / 100 IRE, matching the waveform scope.
        const double ys = H > 1 ? static_cast<double>(H - 1) / 255.0 : 0.0;
        p.setPen(QPen(QColor(120, 120, 120, 120), 1, Qt::DashLine));
        p.drawLine(panel.left(), target.top() + H - 1, panel.right(), target.top() + H - 1);
        p.drawLine(panel.left(),
                   target.top() + H - 1 - static_cast<int>(128 * ys),
                   panel.right(),
                   target.top() + H - 1 - static_cast<int>(128 * ys));
        p.drawLine(panel.left(), target.top(), panel.right(), target.top());
    }
}

} // namespace

QImage renderRgbParadeImage(const RgbParadeData &data, int width, int height)
{
    if (width <= 0 || height <= 0)
        return QImage();

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(20, 20, 22));
    QPainter p(&image);
    paintRgbParadeData(p, QRect(0, 0, width, height), data);
    return image;
}

// ----- HistogramScope ------------------------------------------------------

class HistogramScope : public IScope
{
public:
    QString name() const override { return QStringLiteral("Histogram"); }
    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool e) override { m_enabled = e; }

    void updateFromFrame(const QImage &frame) override {
        const QImage src = downsampleToBudget(frame).convertToFormat(QImage::Format_RGB32);
        std::fill(std::begin(m_r), std::end(m_r), 0);
        std::fill(std::begin(m_g), std::end(m_g), 0);
        std::fill(std::begin(m_b), std::end(m_b), 0);
        for (int y = 0; y < src.height(); ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
            for (int x = 0; x < src.width(); ++x) {
                const QRgb px = line[x];
                ++m_r[qRed(px)];
                ++m_g[qGreen(px)];
                ++m_b[qBlue(px)];
            }
        }
        m_peak = 0;
        for (int i = 0; i < 256; ++i)
            m_peak = qMax(m_peak, qMax(m_r[i], qMax(m_g[i], m_b[i])));
    }

    void paint(QPainter &p, const QRect &target) override {
        p.fillRect(target, QColor(20, 20, 22));
        if (m_peak <= 0) return;
        const double xs = static_cast<double>(target.width()) / 256.0;
        const double ys = static_cast<double>(target.height()) / static_cast<double>(m_peak);
        auto draw = [&](const int *bins, QColor c) {
            c.setAlpha(170);
            p.setPen(QPen(c, 1));
            for (int i = 0; i < 256; ++i) {
                const int h = static_cast<int>(bins[i] * ys);
                const int x = static_cast<int>(i * xs);
                p.drawLine(x, target.height(), x, target.height() - h);
            }
        };
        draw(m_r, QColor(230,  70,  70));
        draw(m_g, QColor( 80, 220,  90));
        draw(m_b, QColor( 90, 130, 240));
        p.setPen(QColor(180, 180, 180));
        p.drawText(4, 12, "Histogram");
    }

private:
    int m_r[256] = {};
    int m_g[256] = {};
    int m_b[256] = {};
    int m_peak = 0;
    bool m_enabled = true;
};

// ----- WaveformScope -------------------------------------------------------

class WaveformScope : public IScope
{
public:
    QString name() const override { return QStringLiteral("Luma Waveform"); }
    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool e) override { m_enabled = e; }

    void updateFromFrame(const QImage &frame) override {
        m_src = downsampleToBudget(frame).convertToFormat(QImage::Format_RGB32);
    }

    void paint(QPainter &p, const QRect &target) override {
        p.fillRect(target, QColor(20, 20, 22));
        if (m_src.isNull()) return;
        // X axis = source column (mapped to widget width). Y axis = luma
        // 0 (bottom) .. 255 (top). Each pixel of the downsampled source
        // contributes one point.
        const int W = target.width();
        const int H = target.height();
        if (W <= 0 || H <= 0) return;
        const double xs = static_cast<double>(W) / static_cast<double>(m_src.width());
        const double ys = static_cast<double>(H) / 255.0;
        p.setPen(QPen(QColor(220, 220, 220, 150), 1));
        for (int y = 0; y < m_src.height(); ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(m_src.constScanLine(y));
            for (int x = 0; x < m_src.width(); ++x) {
                const QRgb px = line[x];
                const int luma = qRound(0.299 * qRed(px)
                                        + 0.587 * qGreen(px)
                                        + 0.114 * qBlue(px));
                const int sx = static_cast<int>(x * xs);
                const int sy = H - 1 - static_cast<int>(luma * ys);
                p.drawPoint(sx, sy);
            }
        }
        // Reference lines at 0 / 50 / 100 IRE.
        p.setPen(QPen(QColor(120, 120, 120, 120), 1, Qt::DashLine));
        p.drawLine(0, H - 1, W, H - 1);
        p.drawLine(0, H - 1 - static_cast<int>(128 * ys), W, H - 1 - static_cast<int>(128 * ys));
        p.drawLine(0, 0, W, 0);
        p.setPen(QColor(180, 180, 180));
        p.drawText(4, 12, "Luma Waveform");
    }

private:
    QImage m_src;
    bool m_enabled = true;
};

// ----- RgbParadeScope ------------------------------------------------------

class RgbParadeScope : public IScope
{
public:
    RgbParadeScope() = default;

    QString name() const override { return QStringLiteral("RGB Parade"); }
    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool e) override { m_enabled = e; }

    void updateFromFrame(const QImage &frame) override {
        m_data = computeRgbParadeData(frame);
    }

    void paint(QPainter &p, const QRect &target) override {
        paintRgbParadeData(p, target, m_data);

        const int W = target.width();
        if (W <= 0 || target.height() <= 0)
            return;

        p.setPen(QColor(180, 180, 180));
        p.drawText(4, 12, "RGB Parade");

        const QColor label[3] = {
            QColor(235, 80, 80),
            QColor(80, 225, 95),
            QColor(95, 135, 245),
        };
        static const char *name[3] = {"R", "G", "B"};
        for (int c = 0; c < 3; ++c) {
            const int panelLeft = target.left() + (c * W) / 3;
            p.setPen(label[c]);
            p.drawText(panelLeft + 4, 26, QString::fromLatin1(name[c]));
        }
    }

private:
    RgbParadeData m_data;
    bool m_enabled = false;
};

// ----- VectorscopeScope ----------------------------------------------------

class VectorscopeScope : public IScope
{
public:
    VectorscopeScope()
        : m_densityBuf(256, 256, QImage::Format_ARGB32_Premultiplied) {}

    QString name() const override { return QStringLiteral("Vectorscope"); }
    bool isEnabled() const override { return m_enabled; }
    void setEnabled(bool e) override { m_enabled = e; }

    void updateFromFrame(const QImage &frame) override {
        m_src = downsampleForVector(frame);
    }

    void paint(QPainter &p, const QRect &target) override {
        p.fillRect(target, QColor(20, 20, 22));
        if (m_src.isNull()) return;

        const int W = target.width();
        const int H = target.height();
        const int sz = qMin(W, H);
        const int cx = W / 2;
        const int cy = H / 2;
        const int radius = sz / 2 - 16;

        // --- 1. Build density accumulation image (256×256) ---------------
        constexpr int kDensSz = 256;
        constexpr int kAlpha = 60;

        m_densityBuf.fill(Qt::transparent);
        QPainter dp(&m_densityBuf);
        dp.setRenderHint(QPainter::Antialiasing, false);
        dp.setPen(Qt::NoPen);
        dp.setBrush(QColor(255, 255, 255, kAlpha));

        // Map [Cb/Cr - 128] ∈ [-128, 128]  →  [0, kDensSz)
        const double mapS = static_cast<double>(kDensSz) / 256.0;

        for (int y = 0; y < m_src.height(); ++y) {
            const QRgb *line =
                reinterpret_cast<const QRgb *>(m_src.constScanLine(y));
            for (int x = 0; x < m_src.width(); ++x) {
                const QRgb px = line[x];
                float fy, fcb, fcr;
                LumetriColor::rgbToYCbCr709(qRed(px), qGreen(px),
                                            qBlue(px), fy, fcb, fcr);

                // Cb → x (horizontal); Cr → y (inverted for screen-up)
                const double nx = (fcb - 128.0) * mapS + kDensSz / 2.0;
                const double ny = -(fcr - 128.0) * mapS + kDensSz / 2.0;

                dp.drawEllipse(QPointF(nx, ny), 2.0, 2.0);
            }
        }
        dp.end();

        // --- 2. Blit density image to widget centre -----------------------
        const QRect destRect(cx - radius, cy - radius, radius * 2, radius * 2);
        p.drawImage(destRect, m_densityBuf);

        // --- 3. Graticule: 75 %-saturation hexagon + target boxes ---------
        struct Target {
            const char *label;
            int r, g, b;  // 75 % colour-bar values
        };
        static const Target kTargets[] = {
            {"R",  191, 0,   0  },
            {"Yl", 191, 191, 0  },
            {"G",  0,   191, 0  },
            {"Cy", 0,   191, 191},
            {"B",  0,   0,   191},
            {"Mg", 191, 0,   191},
        };

        QPointF targetPts[6];
        for (int i = 0; i < 6; ++i) {
            float fy, fcb, fcr;
            LumetriColor::rgbToYCbCr709(kTargets[i].r, kTargets[i].g,
                                        kTargets[i].b, fy, fcb, fcr);
            const double tx = cx + (fcb - 128.0) / 128.0 * radius;
            const double ty = cy - (fcr - 128.0) / 128.0 * radius;
            targetPts[i] = QPointF(tx, ty);
        }

        // Hexagon outline
        p.setPen(QPen(QColor(120, 120, 120, 100), 1, Qt::DashLine));
        for (int i = 0; i < 6; ++i)
            p.drawLine(targetPts[i], targetPts[(i + 1) % 6]);

        // Target boxes
        constexpr int kBox = 7;
        p.setPen(QPen(QColor(200, 200, 200, 160), 1));
        for (int i = 0; i < 6; ++i) {
            p.drawRect(QRectF(targetPts[i].x() - kBox / 2.0,
                              targetPts[i].y() - kBox / 2.0, kBox, kBox));
            // Label
            p.drawText(QPointF(targetPts[i].x() + 5, targetPts[i].y() + 4),
                       QString::fromLatin1(kTargets[i].label));
        }

        // --- 4. Skin-tone line at +33° from +Cb axis ---------------------
        constexpr double kSkinDeg = 33.0;
        const double skinRad = kSkinDeg * M_PI / 180.0;
        const double skDx = std::cos(skinRad) * radius;
        const double skDy = -std::sin(skinRad) * radius;  // flip for screen-Y
        p.setPen(QPen(QColor(220, 180, 140, 130), 1, Qt::DotLine));
        p.drawLine(QPointF(cx, cy), QPointF(cx + skDx, cy + skDy));

        // --- 5. Crosshair reference ---------------------------------------
        p.setPen(QPen(QColor(120, 120, 120, 100), 1, Qt::DashLine));
        p.drawLine(QPointF(cx - radius, cy), QPointF(cx + radius, cy));
        p.drawLine(QPointF(cx, cy - radius), QPointF(cx, cy + radius));

        // --- 6. Label -----------------------------------------------------
        p.setPen(QColor(180, 180, 180));
        p.drawText(4, 12, "Vectorscope");
    }

private:
    QImage m_src;
    QImage m_densityBuf;
    bool m_enabled = true;
};

// ----- ScopeCanvas ----------------------------------------------------------

class ScopeCanvas : public QWidget
{
public:
    ScopeCanvas(QWidget *parent, const std::vector<std::unique_ptr<IScope>> &scopes)
        : QWidget(parent), m_scopes(scopes)
    {
        setMinimumSize(200, 300);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(20, 20, 22));

        int n = 0;
        for (const auto &s : m_scopes)
            if (s->isEnabled()) ++n;
        if (n == 0) return;

        int y = 0;
        const int h = height() / n;
        for (const auto &s : m_scopes) {
            if (!s->isEnabled()) continue;
            p.save();
            p.translate(0, y);
            s->paint(p, QRect(0, 0, width(), h));
            p.restore();
            y += h;
        }
    }

private:
    const std::vector<std::unique_ptr<IScope>> &m_scopes;
};

// ----- LumetriScopes container --------------------------------------------

LumetriScopes::LumetriScopes(QWidget *parent) : QWidget(parent)
{
    m_scopes.push_back(std::make_unique<HistogramScope>());
    m_scopes.push_back(std::make_unique<WaveformScope>());
    m_scopes.push_back(std::make_unique<VectorscopeScope>());
    m_scopes.push_back(std::make_unique<RgbParadeScope>());

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(4);

    m_canvas = new ScopeCanvas(this, m_scopes);
    layout->addWidget(m_canvas, 1);

    // Toggle row
    auto *toggleRow = new QHBoxLayout();
    toggleRow->setContentsMargins(4, 0, 4, 0);

    for (auto &s : m_scopes) {
        auto *cb = new QCheckBox(s->name(), this);
        cb->setChecked(s->isEnabled());
        toggleRow->addWidget(cb);
        m_toggles[s->name()] = cb;
        connect(cb, &QCheckBox::toggled, this,
                [this, raw = s.get()](bool checked) {
            raw->setEnabled(checked);
            m_canvas->update();
        });
    }
    toggleRow->addStretch();
    layout->addLayout(toggleRow);

    m_throttle.start();
}

void LumetriScopes::setFrame(const QImage &frame)
{
    if (frame.isNull()) return;
    if (!isVisible()) return;
    if (m_throttle.elapsed() < kThrottleMs) return;
    m_throttle.restart();
    for (auto &s : m_scopes) {
        if (s->isEnabled())
            s->updateFromFrame(frame);
    }
    m_canvas->update();
}
