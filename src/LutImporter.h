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

    bool isValid() const { return size > 0 && !table.isEmpty(); }
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
