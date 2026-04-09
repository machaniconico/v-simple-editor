#include "LutImporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextStream>
#include <QRegularExpression>
#include <QPainter>
#include <QtMath>
#include <algorithm>
#include <cmath>

static inline int clamp255(int v) { return qBound(0, v, 255); }

// ===== LutImporter =====

LutData LutImporter::loadCubeFile(const QString &filePath)
{
    LutData lut;
    lut.filePath = filePath;
    lut.type = LutType::Cube3D;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return lut;

    QFileInfo fi(filePath);
    lut.name = fi.completeBaseName();

    QTextStream in(&file);
    bool is1D = false;

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();

        // Skip empty lines and comments
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        // Parse TITLE
        if (line.startsWith("TITLE")) {
            // TITLE "Some Name"
            int q1 = line.indexOf('"');
            int q2 = line.lastIndexOf('"');
            if (q1 >= 0 && q2 > q1)
                lut.name = line.mid(q1 + 1, q2 - q1 - 1);
            continue;
        }

        // Parse LUT size
        if (line.startsWith("LUT_3D_SIZE")) {
            lut.size = line.mid(11).trimmed().toInt();
            lut.type = LutType::Cube3D;
            is1D = false;
            continue;
        }
        if (line.startsWith("LUT_1D_SIZE")) {
            lut.size = line.mid(11).trimmed().toInt();
            lut.type = LutType::Cube1D;
            is1D = true;
            continue;
        }

        // Skip DOMAIN_MIN / DOMAIN_MAX and other keywords
        if (line.startsWith("DOMAIN_MIN") || line.startsWith("DOMAIN_MAX"))
            continue;
        if (line[0].isLetter())
            continue;

        // Parse RGB triplet
        QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            bool okR, okG, okB;
            float r = parts[0].toFloat(&okR);
            float g = parts[1].toFloat(&okG);
            float b = parts[2].toFloat(&okB);
            if (okR && okG && okB)
                lut.table.append(QVector3D(r, g, b));
        }
    }

    // Validate table size
    if (lut.type == LutType::Cube3D) {
        int expected = lut.size * lut.size * lut.size;
        if (lut.table.size() != expected) {
            lut.size = 0;
            lut.table.clear();
        }
    } else {
        if (lut.table.size() != lut.size) {
            lut.size = 0;
            lut.table.clear();
        }
    }

    return lut;
}

QVector3D LutImporter::trilinearInterpolate(const QVector<QVector3D> &table, int size,
                                             float r, float g, float b)
{
    // Scale input (0-1) to LUT grid coordinates
    float maxIdx = static_cast<float>(size - 1);
    float rIdx = qBound(0.0f, r * maxIdx, maxIdx);
    float gIdx = qBound(0.0f, g * maxIdx, maxIdx);
    float bIdx = qBound(0.0f, b * maxIdx, maxIdx);

    // Integer and fractional parts
    int r0 = qMin(static_cast<int>(rIdx), size - 2);
    int g0 = qMin(static_cast<int>(gIdx), size - 2);
    int b0 = qMin(static_cast<int>(bIdx), size - 2);
    int r1 = r0 + 1;
    int g1 = g0 + 1;
    int b1 = b0 + 1;

    float rFrac = rIdx - r0;
    float gFrac = gIdx - g0;
    float bFrac = bIdx - b0;

    // .cube 3D layout: R changes fastest, then G, then B
    // index = r + g * size + b * size * size
    auto idx = [&](int ri, int gi, int bi) -> int {
        return ri + gi * size + bi * size * size;
    };

    // Fetch 8 corners of the cube
    QVector3D c000 = table[idx(r0, g0, b0)];
    QVector3D c100 = table[idx(r1, g0, b0)];
    QVector3D c010 = table[idx(r0, g1, b0)];
    QVector3D c110 = table[idx(r1, g1, b0)];
    QVector3D c001 = table[idx(r0, g0, b1)];
    QVector3D c101 = table[idx(r1, g0, b1)];
    QVector3D c011 = table[idx(r0, g1, b1)];
    QVector3D c111 = table[idx(r1, g1, b1)];

    // Trilinear interpolation
    QVector3D c00 = c000 * (1.0f - rFrac) + c100 * rFrac;
    QVector3D c10 = c010 * (1.0f - rFrac) + c110 * rFrac;
    QVector3D c01 = c001 * (1.0f - rFrac) + c101 * rFrac;
    QVector3D c11 = c011 * (1.0f - rFrac) + c111 * rFrac;

    QVector3D c0 = c00 * (1.0f - gFrac) + c10 * gFrac;
    QVector3D c1 = c01 * (1.0f - gFrac) + c11 * gFrac;

    return c0 * (1.0f - bFrac) + c1 * bFrac;
}

QImage LutImporter::applyLut(const QImage &image, const LutData &lutData)
{
    if (!lutData.isValid())
        return image;

    QImage img = image.convertToFormat(QImage::Format_RGB888);
    int w = img.width(), h = img.height();

    if (lutData.type == LutType::Cube3D) {
        for (int y = 0; y < h; ++y) {
            uint8_t *line = img.scanLine(y);
            for (int x = 0; x < w; ++x) {
                float r = line[x * 3]     / 255.0f;
                float g = line[x * 3 + 1] / 255.0f;
                float b = line[x * 3 + 2] / 255.0f;

                QVector3D mapped = trilinearInterpolate(lutData.table, lutData.size, r, g, b);

                line[x * 3]     = clamp255(static_cast<int>(mapped.x() * 255.0f));
                line[x * 3 + 1] = clamp255(static_cast<int>(mapped.y() * 255.0f));
                line[x * 3 + 2] = clamp255(static_cast<int>(mapped.z() * 255.0f));
            }
        }
    } else {
        // 1D LUT — apply per-channel independently
        for (int y = 0; y < h; ++y) {
            uint8_t *line = img.scanLine(y);
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 3; ++c) {
                    float val = line[x * 3 + c] / 255.0f;
                    float idx = qBound(0.0f, val * (lutData.size - 1), static_cast<float>(lutData.size - 2));
                    int i0 = static_cast<int>(idx);
                    int i1 = i0 + 1;
                    float frac = idx - i0;

                    float v0 = (c == 0) ? lutData.table[i0].x()
                             : (c == 1) ? lutData.table[i0].y()
                                        : lutData.table[i0].z();
                    float v1 = (c == 0) ? lutData.table[i1].x()
                             : (c == 1) ? lutData.table[i1].y()
                                        : lutData.table[i1].z();
                    float mapped = v0 * (1.0f - frac) + v1 * frac;
                    line[x * 3 + c] = clamp255(static_cast<int>(mapped * 255.0f));
                }
            }
        }
    }

    return img;
}

QImage LutImporter::applyLutWithIntensity(const QImage &image, const LutData &lutData,
                                           double intensity)
{
    if (!lutData.isValid() || intensity <= 0.0)
        return image;

    if (intensity >= 1.0)
        return applyLut(image, lutData);

    QImage original = image.convertToFormat(QImage::Format_RGB888);
    QImage lutApplied = applyLut(image, lutData);

    int w = original.width(), h = original.height();
    float t = static_cast<float>(intensity);

    for (int y = 0; y < h; ++y) {
        const uint8_t *origLine = original.constScanLine(y);
        uint8_t *lutLine = lutApplied.scanLine(y);
        for (int x = 0; x < w * 3; ++x) {
            float orig = origLine[x];
            float lut  = lutLine[x];
            lutLine[x] = clamp255(static_cast<int>(orig * (1.0f - t) + lut * t));
        }
    }

    return lutApplied;
}

QImage LutImporter::generatePreview(const LutData &lutData, int previewSize)
{
    // Create a colour gradient image: horizontal = hue, vertical = brightness
    QImage gradient(previewSize, previewSize, QImage::Format_RGB888);

    for (int y = 0; y < previewSize; ++y) {
        uint8_t *line = gradient.scanLine(y);
        float brightness = 1.0f - static_cast<float>(y) / (previewSize - 1);
        for (int x = 0; x < previewSize; ++x) {
            float hue = static_cast<float>(x) / (previewSize - 1);

            // Convert HSV to RGB (saturation = 0.8, value = brightness)
            float h = hue * 360.0f;
            float s = 0.8f;
            float v = brightness;

            float c = v * s;
            float hh = h / 60.0f;
            float xc = c * (1.0f - std::fabs(std::fmod(hh, 2.0f) - 1.0f));
            float m = v - c;

            float r = 0, g = 0, b = 0;
            if      (hh < 1) { r = c; g = xc; }
            else if (hh < 2) { r = xc; g = c; }
            else if (hh < 3) { g = c; b = xc; }
            else if (hh < 4) { g = xc; b = c; }
            else if (hh < 5) { r = xc; b = c; }
            else              { r = c; b = xc; }

            line[x * 3]     = clamp255(static_cast<int>((r + m) * 255.0f));
            line[x * 3 + 1] = clamp255(static_cast<int>((g + m) * 255.0f));
            line[x * 3 + 2] = clamp255(static_cast<int>((b + m) * 255.0f));
        }
    }

    // Apply the LUT to the gradient
    return applyLut(gradient, lutData);
}

// ===== LutLibrary =====

LutLibrary &LutLibrary::instance()
{
    static LutLibrary lib;
    return lib;
}

LutLibrary::LutLibrary()
{
    registerBuiltins();
    loadPaths();
}

// --- Query ---

QVector<LutData> LutLibrary::allLuts() const
{
    return m_luts;
}

QVector<LutData> LutLibrary::builtInLuts() const
{
    QVector<LutData> result;
    for (const auto &lut : m_luts)
        if (lut.isBuiltIn()) result.append(lut);
    return result;
}

LutData LutLibrary::findByName(const QString &name) const
{
    for (const auto &lut : m_luts)
        if (lut.name == name) return lut;
    return LutData{};
}

// --- Mutate ---

bool LutLibrary::addLut(const QString &filePath)
{
    LutData lut = LutImporter::loadCubeFile(filePath);
    if (!lut.isValid())
        return false;

    // Reject duplicates
    for (const auto &existing : m_luts)
        if (existing.name == lut.name) return false;

    m_luts.append(lut);
    return true;
}

bool LutLibrary::removeLut(const QString &name)
{
    for (int i = 0; i < m_luts.size(); ++i) {
        if (m_luts[i].name == name) {
            if (m_luts[i].isBuiltIn()) return false;   // cannot remove built-in LUTs
            m_luts.removeAt(i);
            return true;
        }
    }
    return false;
}

// --- Persist ---

QString LutLibrary::libraryPath()
{
    QString dir = QDir::homePath() + "/.veditor";
    QDir().mkpath(dir);
    return dir + "/luts.json";
}

bool LutLibrary::savePaths() const
{
    QJsonArray arr;
    for (const auto &lut : m_luts) {
        if (lut.isBuiltIn()) continue;          // only persist user-loaded LUT paths
        QJsonObject obj;
        obj["name"]     = lut.name;
        obj["filePath"] = lut.filePath;
        arr.append(obj);
    }

    QFile file(libraryPath());
    if (!file.open(QIODevice::WriteOnly)) return false;

    QJsonDocument doc(arr);
    file.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

bool LutLibrary::loadPaths()
{
    QFile file(libraryPath());
    if (!file.exists()) return true;            // first run — nothing to load
    if (!file.open(QIODevice::ReadOnly)) return false;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError) return false;

    const QJsonArray arr = doc.array();
    for (const auto &v : arr) {
        QJsonObject obj = v.toObject();
        QString path = obj["filePath"].toString();
        if (path.isEmpty()) continue;

        // Skip if a LUT with the same name already exists
        QString name = obj["name"].toString();
        bool exists = false;
        for (const auto &lut : m_luts) {
            if (lut.name == name) { exists = true; break; }
        }
        if (!exists)
            addLut(path);
    }
    return true;
}

// ===== Built-in LUT generators =====

void LutLibrary::registerBuiltins()
{
    m_luts.append(generateFilmEmulation());
    m_luts.append(generateTealOrange());
    m_luts.append(generateFadedFilm());
    m_luts.append(generateHighContrast());
}

// --- Film Emulation: warm shadows, cool highlights ---
LutData LutLibrary::generateFilmEmulation()
{
    const int sz = 17;
    LutData lut;
    lut.name = "Film Emulation";
    lut.type = LutType::Cube3D;
    lut.size = sz;
    lut.table.resize(sz * sz * sz);

    for (int b = 0; b < sz; ++b) {
        for (int g = 0; g < sz; ++g) {
            for (int r = 0; r < sz; ++r) {
                float rf = static_cast<float>(r) / (sz - 1);
                float gf = static_cast<float>(g) / (sz - 1);
                float bf = static_cast<float>(b) / (sz - 1);

                float luma = 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;

                // Warm shadows: add red/yellow to dark areas
                float shadowWeight = 1.0f - qBound(0.0f, luma * 2.0f, 1.0f);
                float rOut = rf + shadowWeight * 0.06f;
                float gOut = gf + shadowWeight * 0.03f;
                float bOut = bf - shadowWeight * 0.02f;

                // Cool highlights: add blue to bright areas
                float highlightWeight = qBound(0.0f, (luma - 0.5f) * 2.0f, 1.0f);
                rOut -= highlightWeight * 0.03f;
                gOut -= highlightWeight * 0.01f;
                bOut += highlightWeight * 0.05f;

                // Slight S-curve for film-like contrast
                auto sCurve = [](float x) -> float {
                    return x + 0.1f * std::sin(x * 3.14159f);
                };
                rOut = sCurve(qBound(0.0f, rOut, 1.0f));
                gOut = sCurve(qBound(0.0f, gOut, 1.0f));
                bOut = sCurve(qBound(0.0f, bOut, 1.0f));

                int idx = r + g * sz + b * sz * sz;
                lut.table[idx] = QVector3D(
                    qBound(0.0f, rOut, 1.0f),
                    qBound(0.0f, gOut, 1.0f),
                    qBound(0.0f, bOut, 1.0f));
            }
        }
    }
    return lut;
}

// --- Teal & Orange: classic cinema look ---
LutData LutLibrary::generateTealOrange()
{
    const int sz = 17;
    LutData lut;
    lut.name = "Teal & Orange";
    lut.type = LutType::Cube3D;
    lut.size = sz;
    lut.table.resize(sz * sz * sz);

    for (int b = 0; b < sz; ++b) {
        for (int g = 0; g < sz; ++g) {
            for (int r = 0; r < sz; ++r) {
                float rf = static_cast<float>(r) / (sz - 1);
                float gf = static_cast<float>(g) / (sz - 1);
                float bf = static_cast<float>(b) / (sz - 1);

                float luma = 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;

                // Skin/warm tones push towards orange
                float warmth = qBound(0.0f, (rf - bf) * 0.5f + 0.5f, 1.0f);
                float orangeWeight = warmth * 0.15f;
                float tealWeight = (1.0f - warmth) * 0.15f;

                float rOut = rf + orangeWeight - tealWeight * 0.3f;
                float gOut = gf - orangeWeight * 0.2f + tealWeight * 0.1f;
                float bOut = bf - orangeWeight * 0.5f + tealWeight;

                // Boost saturation slightly
                float sat = 1.1f;
                rOut = luma + (rOut - luma) * sat;
                gOut = luma + (gOut - luma) * sat;
                bOut = luma + (bOut - luma) * sat;

                int idx = r + g * sz + b * sz * sz;
                lut.table[idx] = QVector3D(
                    qBound(0.0f, rOut, 1.0f),
                    qBound(0.0f, gOut, 1.0f),
                    qBound(0.0f, bOut, 1.0f));
            }
        }
    }
    return lut;
}

// --- Faded Film: lifted blacks, muted colours ---
LutData LutLibrary::generateFadedFilm()
{
    const int sz = 17;
    LutData lut;
    lut.name = "Faded Film";
    lut.type = LutType::Cube3D;
    lut.size = sz;
    lut.table.resize(sz * sz * sz);

    for (int b = 0; b < sz; ++b) {
        for (int g = 0; g < sz; ++g) {
            for (int r = 0; r < sz; ++r) {
                float rf = static_cast<float>(r) / (sz - 1);
                float gf = static_cast<float>(g) / (sz - 1);
                float bf = static_cast<float>(b) / (sz - 1);

                float luma = 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;

                // Lift blacks — raise the floor
                float lift = 0.08f;
                float rOut = rf * (1.0f - lift) + lift;
                float gOut = gf * (1.0f - lift) + lift;
                float bOut = bf * (1.0f - lift) + lift;

                // Crush whites slightly
                float ceiling = 0.95f;
                rOut = rOut * ceiling;
                gOut = gOut * ceiling;
                bOut = bOut * ceiling;

                // Desaturate — muted colours
                float desat = 0.7f;
                float lumaOut = 0.2126f * rOut + 0.7152f * gOut + 0.0722f * bOut;
                rOut = lumaOut + (rOut - lumaOut) * desat;
                gOut = lumaOut + (gOut - lumaOut) * desat;
                bOut = lumaOut + (bOut - lumaOut) * desat;

                // Slight warm tint
                rOut += 0.02f;
                bOut -= 0.01f;

                int idx = r + g * sz + b * sz * sz;
                lut.table[idx] = QVector3D(
                    qBound(0.0f, rOut, 1.0f),
                    qBound(0.0f, gOut, 1.0f),
                    qBound(0.0f, bOut, 1.0f));
            }
        }
    }
    return lut;
}

// --- High Contrast: S-curve contrast boost ---
LutData LutLibrary::generateHighContrast()
{
    const int sz = 17;
    LutData lut;
    lut.name = "High Contrast";
    lut.type = LutType::Cube3D;
    lut.size = sz;
    lut.table.resize(sz * sz * sz);

    // S-curve function for strong contrast
    auto sCurve = [](float x) -> float {
        // Stronger sigmoid-like S-curve
        float t = qBound(0.0f, x, 1.0f);
        return t * t * (3.0f - 2.0f * t);   // smoothstep
    };

    for (int b = 0; b < sz; ++b) {
        for (int g = 0; g < sz; ++g) {
            for (int r = 0; r < sz; ++r) {
                float rf = static_cast<float>(r) / (sz - 1);
                float gf = static_cast<float>(g) / (sz - 1);
                float bf = static_cast<float>(b) / (sz - 1);

                // Apply S-curve per channel
                float rOut = sCurve(rf);
                float gOut = sCurve(gf);
                float bOut = sCurve(bf);

                // Boost saturation slightly to compensate for contrast
                float luma = 0.2126f * rOut + 0.7152f * gOut + 0.0722f * bOut;
                float sat = 1.15f;
                rOut = luma + (rOut - luma) * sat;
                gOut = luma + (gOut - luma) * sat;
                bOut = luma + (bOut - luma) * sat;

                int idx = r + g * sz + b * sz * sz;
                lut.table[idx] = QVector3D(
                    qBound(0.0f, rOut, 1.0f),
                    qBound(0.0f, gOut, 1.0f),
                    qBound(0.0f, bOut, 1.0f));
            }
        }
    }
    return lut;
}
