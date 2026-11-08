#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QVector3D>
#include <QImage>

// --- LUT Types ---

enum class LutType {
    Cube3D,
    Cube1D
};

// --- LUT Data ---

struct LutData {
    QString name;
    QString filePath;
    LutType type = LutType::Cube3D;

    int size = 0;                    // grid size (e.g. 33 for 33x33x33)
    QVector<QVector3D> table;        // RGB triplets (0.0-1.0)
    double intensity = 1.0;          // blend strength with original (0.0-1.0)
    QVector3D domainMin{0.0f, 0.0f, 0.0f};  // DOMAIN_MIN of the .cube (input is normalised by it)
    QVector3D domainMax{1.0f, 1.0f, 1.0f};  // DOMAIN_MAX of the .cube

    bool isValid() const { return size >= 2 && !table.isEmpty(); }
    bool hasDefaultDomain() const {
        return domainMin == QVector3D(0.0f, 0.0f, 0.0f) && domainMax == QVector3D(1.0f, 1.0f, 1.0f);
    }
    bool isBuiltIn() const { return filePath.isEmpty(); }
};

// --- LUT Importer ---

class LutImporter
{
public:
    // Parse a .cube file and return populated LutData
    static LutData loadCubeFile(const QString &filePath);

    // Apply LUT to an image using trilinear interpolation (full intensity)
    static QImage applyLut(const QImage &image, const LutData &lutData);

    // Apply LUT blended with original at given intensity (0.0 = original, 1.0 = full LUT)
    static QImage applyLutWithIntensity(const QImage &image, const LutData &lutData,
                                        double intensity);

    // Float sampler shared by applyLut and toGpuGrid: normalises rgb01 by the LUT domain
    // (clamped to 0..1 per channel), then 1D linear / 3D trilinear lookup. lut must be valid.
    static QVector3D sample(const LutData &lut, const QVector3D &rgb01);

    // Grid for the GL preview (a 3D texture of size^3). A 1D LUT is expanded to an n^3 grid
    // (n = clamp(size, 2, maxSize)) through sample(); a 3D LUT is returned unchanged, whatever
    // its domain (the shader remaps by DOMAIN_MIN / DOMAIN_MAX before the lookup).
    static LutData toGpuGrid(const LutData &in, int maxSize = 65);

    // CPU twins of the GL preview's 3D LUT lookup (GLPreview's fragment shader uses the same
    // formulas): the domain normalisation (rgb01 - domainMin) / (domainMax - domainMin) clamped
    // to 0..1 per channel, then the texture coordinate (clamp(x, 0, 1) * (n - 1) + 0.5) / n that
    // puts knot 0 .. n - 1 at the centre of texel 0 .. n - 1 of an n-texel axis.
    static QVector3D gpuDomainRemap(const LutData &lut, const QVector3D &rgb01);
    static float gpuTexelCoord(int n, float x);

    // Generate a preview image showing the LUT effect on a colour gradient
    static QImage generatePreview(const LutData &lutData, int previewSize = 128);

private:
    // Trilinear interpolation lookup in a 3D LUT table
    static QVector3D trilinearInterpolate(const QVector<QVector3D> &table, int size,
                                          float r, float g, float b);
};

// --- LUT Library (singleton) ---

class LutLibrary
{
public:
    static LutLibrary &instance();

    // Query
    QVector<LutData> allLuts() const;
    QVector<LutData> builtInLuts() const;
    LutData findByName(const QString &name) const;

    // Mutate
    bool addLut(const QString &filePath);
    bool removeLut(const QString &name);

    // Persist loaded LUT file paths to ~/.veditor/luts.json
    bool savePaths() const;
    bool loadPaths();

    int count() const { return m_luts.size(); }

private:
    LutLibrary();
    void registerBuiltins();

    static QString libraryPath();

    // Built-in LUT generators
    static LutData generateFilmEmulation();
    static LutData generateTealOrange();
    static LutData generateFadedFilm();
    static LutData generateHighContrast();

    QVector<LutData> m_luts;
};
