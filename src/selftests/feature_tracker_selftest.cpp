#include "../FeatureTracker.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>

namespace {
QImage texture(int width, int height, int rectangles)
{
    QImage image(width, height, QImage::Format_Grayscale8);
    image.fill(96);
    std::mt19937 rng(12345);
    for (int i = 0; i < rectangles; ++i) {
        const int w = 8 + int(rng() % 33), h = 8 + int(rng() % 33);
        // Leave room for the rotated image and a complete tracking window.
        const int x = 48 + int(rng() % (width - w - 96));
        const int y = 48 + int(rng() % (height - h - 96));
        const unsigned char value = static_cast<unsigned char>(rng() % 256);
        for (int py = y; py < y + h; ++py)
            for (int px = x; px < x + w; ++px) image.scanLine(py)[px] = value;
    }
    return image;
}

struct Affine {
    double a, b, dx, dy, cx, cy;
    QPointF map(const QPointF& p) const {
        const double x = p.x() - cx, y = p.y() - cy;
        return QPointF(a*x - b*y + cx + dx, b*x + a*y + cy + dy);
    }
};

QImage warp(const QImage& source, const Affine& transform)
{
    QImage out(source.size(), QImage::Format_Grayscale8);
    const double det = transform.a*transform.a + transform.b*transform.b;
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) {
            const double u = x - transform.cx - transform.dx;
            const double v = y - transform.cy - transform.dy;
            const double sx = (transform.a*u + transform.b*v)/det + transform.cx;
            const double sy = (-transform.b*u + transform.a*v)/det + transform.cy;
            double value = 96;
            if (sx >= 0 && sy >= 0 && sx < source.width()-1 && sy < source.height()-1) {
                const int ix = int(sx), iy = int(sy);
                const double fx = sx - ix, fy = sy - iy;
                const auto* row = source.constScanLine(iy);
                const auto* below = source.constScanLine(iy+1);
                value = (1-fy)*((1-fx)*row[ix]+fx*row[ix+1])
                      + fy*((1-fx)*below[ix]+fx*below[ix+1]);
            }
            out.scanLine(y)[x] = static_cast<unsigned char>(std::lround(value));
        }
    return out;
}

QVector<feattrack::Track> makeTracks(const QVector<QPointF>& points)
{
    QVector<feattrack::Track> tracks;
    for (const auto& p : points) tracks.push_back({p, 0.0f, true});
    return tracks;
}

int accurate(const QVector<QPointF>& original, const QVector<feattrack::Track>& tracks,
             const Affine& transform, double tolerance)
{
    int good = 0;
    for (qsizetype i = 0; i < original.size(); ++i) {
        const QPointF expected = transform.map(original[i]);
        if (tracks[i].alive && std::isfinite(tracks[i].error)
            && std::hypot(tracks[i].pt.x()-expected.x(), tracks[i].pt.y()-expected.y()) <= tolerance)
            ++good;
    }
    return good;
}
} // namespace

int runFeatureTrackerSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::cerr << (ok ? "PASS G" : "FAIL G") << number << '\n';
        if (ok) ++passed; else ++failed;
    };
    const QImage image = texture(640, 480, 200);
    const auto points = feattrack::detectShiTomasi(image, 500, 0.01, 8);
    QImage flat(image.size(), QImage::Format_Grayscale8);
    flat.fill(96);
    std::cerr << "detected: " << points.size() << '\n';
    gate(1, points.size() >= 100 && points.size() <= 500
         && feattrack::detectShiTomasi(flat).isEmpty()
         && feattrack::detectShiTomasi(QImage()).isEmpty()
         && feattrack::detectShiTomasi(image, 0).isEmpty()
         && feattrack::detectShiTomasi(image) == points);
    bool spaced = !points.isEmpty();
    for (qsizetype i = 0; i < points.size(); ++i)
        for (qsizetype j = i + 1; j < points.size(); ++j)
            spaced = spaced && std::hypot(points[i].x()-points[j].x(), points[i].y()-points[j].y()) >= 8;
    gate(2, spaced);

    const Affine translation{1, 0, 3.7, -2.2, 320, 240};
    const QImage translated = warp(image, translation);
    auto tracks = makeTracks(points);
    feattrack::trackPyramidalLK(image, translated, tracks);
    const int translatedGood = accurate(points, tracks, translation, 0.3);
    std::cerr << "translation: " << translatedGood << '/' << points.size() << '\n';
    // Denominator includes rejected points: dropping hard matches cannot pass accuracy.
    auto unchecked = makeTracks(points);
    feattrack::trackPyramidalLK(image, translated, unchecked, 3, 15, 20, 0.01, false);
    gate(3, !points.isEmpty() && translatedGood >= 0.95 * points.size()
         && accurate(points, unchecked, translation, 0.3) >= 0.95 * points.size());

    constexpr double angle = 10.0 * 3.14159265358979323846 / 180.0;
    const Affine rotation{1.05*std::cos(angle), 1.05*std::sin(angle), 0, 0, 320, 240};
    tracks = makeTracks(points);
    feattrack::trackPyramidalLK(image, warp(image, rotation), tracks);
    const int rotatedGood = accurate(points, tracks, rotation, 1.0);
    std::cerr << "rotation: " << rotatedGood << '/' << points.size() << '\n';
    gate(4, !points.isEmpty() && rotatedGood >= 0.80 * points.size());

    QImage occluded = image.copy();
    for (int y = 0; y < occluded.height(); ++y)
        for (int x = 0; x < occluded.width()/2; ++x) occluded.scanLine(y)[x] = 0;
    tracks = makeTracks(points);
    feattrack::trackPyramidalLK(image, occluded, tracks);
    int hidden = 0, rejected = 0, visible = 0, retained = 0;
    for (qsizetype i = 0; i < points.size(); ++i) {
        if (points[i].x() < 320) { ++hidden; if (!tracks[i].alive) ++rejected; }
        if (points[i].x() >= 336) { ++visible; if (tracks[i].alive) ++retained; }
    }
    QVector<feattrack::Track> invalid{{QPointF(-1, 20), 0, true},
        {QPointF(std::numeric_limits<double>::quiet_NaN(), 20), 0, true},
        {QPointF(100, 100), 7, false}};
    feattrack::trackPyramidalLK(image, image, invalid);
    auto badImages = makeTracks(points);
    feattrack::trackPyramidalLK(image, QImage(), badImages);
    bool allRejected = true;
    for (const auto& t : badImages) allRejected = allRejected && !t.alive;
    std::cerr << "occlusion: " << rejected << '/' << hidden
              << ", visible retained: " << retained << '/' << visible << '\n';
    gate(5, hidden > 0 && rejected == hidden && visible > 0 && retained >= 0.95 * visible
         && !invalid[0].alive && !invalid[1].alive && !invalid[2].alive
         && invalid[2].error == 7 && invalid[2].pt == QPointF(100, 100) && allRejected);

#ifdef NDEBUG
    const QImage hd = texture(1920, 1080, 1200);
    const auto hdPoints = feattrack::detectShiTomasi(hd, 500);
    auto hdTracks = makeTracks(hdPoints);
    const QImage hdNext = warp(hd, translation);
    const auto start = std::chrono::steady_clock::now();
    feattrack::trackPyramidalLK(hd, hdNext, hdTracks);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    std::cerr << (ms > 2000 ? "WARN " : "") << "G6 1080p, " << hdPoints.size() << " points: " << ms << " ms\n";
    gate(6, hdPoints.size() == 500);
#else
    std::cerr << "SKIP G6 (Debug)\n";
    gate(6, true);
#endif
    std::cerr << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    return failed;
}
