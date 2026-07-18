#pragma once

#include <QImage>
#include <QWidget>
#include <QElapsedTimer>
#include <QtMath>
#include <array>
#include <memory>
#include <vector>
#include <QHash>

class QCheckBox;
class QPainter;
class QRect;

// BT.709 Y'CbCr conversion helpers for vectorscope rendering.
namespace LumetriColor {
inline void rgbToYCbCr709(int r, int g, int b, float &y, float &cb, float &cr)
{
    y  = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    cb = -0.1146f * r - 0.3854f * g + 0.5000f * b + 128.0f;
    cr =  0.5000f * r - 0.4542f * g - 0.0458f * b + 128.0f;
}

inline void rgbToYCbCr709(int r, int g, int b, int &y, int &cb, int &cr)
{
    float fy, fcb, fcr;
    rgbToYCbCr709(r, g, b, fy, fcb, fcr);
    y  = qBound(0, qRound(fy),  255);
    cb = qBound(0, qRound(fcb), 255);
    cr = qBound(0, qRound(fcr), 255);
}
} // namespace LumetriColor

struct RgbParadeData {
    using ColumnHistogram = std::array<int, 256>;

    std::array<std::vector<ColumnHistogram>, 3> channels;
    int sourceHeight = 0;

    int width() const { return static_cast<int>(channels[0].size()); }
    bool isEmpty() const { return channels[0].empty(); }
};

RgbParadeData computeRgbParadeData(const QImage &frame);
QImage renderRgbParadeImage(const RgbParadeData &data, int width, int height);

// Abstract interface for a measurement scope (Histogram, Waveform,
// Vectorscope, ...). Each scope implements its own data-accumulation
// and painting logic so adding a fourth scope only requires one new
// class and one push_back.
class IScope {
public:
    virtual ~IScope() = default;
    virtual QString name() const = 0;
    virtual void updateFromFrame(const QImage &frame) = 0;
    virtual void paint(QPainter &p, const QRect &target) = 0;
    virtual bool isEnabled() const = 0;
    virtual void setEnabled(bool) = 0;
};

// Lumetri-style measurement scopes (RGB Histogram + Luma Waveform +
// Vectorscope + RGB Parade). Scopes share a single container widget that
// exposes a setFrame() slot.  Frame ingestion is throttled to ~10 fps
// so a 60 fps preview doesn't burn the GUI thread on histogram math.
class LumetriScopes : public QWidget
{
    Q_OBJECT
public:
    explicit LumetriScopes(QWidget *parent = nullptr);

public slots:
    // Drop-in target for VideoPlayer::frameComposited. Internally rate-
    // limits to roughly the kThrottleMs interval — for cheap shipboard
    // monitoring rather than precision colourist work.
    void setFrame(const QImage &frame);

private:
    std::vector<std::unique_ptr<IScope>> m_scopes;
    QHash<QString, QCheckBox*> m_toggles;
    QWidget *m_canvas = nullptr;
    QElapsedTimer m_throttle;
    static constexpr int kThrottleMs = 100;  // ~10 fps refresh
};
