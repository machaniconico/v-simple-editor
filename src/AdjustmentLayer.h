#pragma once

// Premiere/Photoshop-style "Adjustment Layer" — a special timeline clip that
// carries no video content of its own but applies grading/effect parameters
// to every video frame underneath it on the same time range.
//
// THIS STORY IS DATA-MODEL ONLY. The struct + Timeline storage + ProjectFile
// JSON round-trip are wired here so the GLPreview composite path and any
// future MainWindow UI can plug in without churning the schema. The compose
// helper below evaluates the cumulative effect of all overlapping layers at
// a given time and is intentionally cheap (no allocations in the common
// "no layers" path) so it can be called per-frame from the render loop.

#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <QtGlobal>

struct AdjustmentLayer {
    int id = -1;                       // unique, monotonic, persists across save/load
    qint64 timelineStartUs = 0;        // microseconds on timeline (inclusive)
    qint64 timelineEndUs = 0;          // microseconds on timeline (exclusive)
    int trackIndex = 0;                // V-track this adjustment lives on
    QString name;                      // user-friendly display name

    // Grading parameters mirror ColorGradingPanel state. Defaults are the
    // identity transform so a freshly-added layer with `gradingEnabled=false`
    // is a no-op even before the user touches a slider.
    bool gradingEnabled = false;

    // Lift / Gamma / Gain — RGB+Luma. Slider values in the panel's native
    // [-100..+100] (lift/gamma) or panel-defined range. Index order matches
    // ColorGradingPanel: 0=R, 1=G, 2=B, 3=Luma.
    double lift[4]  = {0.0, 0.0, 0.0, 0.0};
    double gamma[4] = {0.0, 0.0, 0.0, 0.0};
    double gain[4]  = {0.0, 0.0, 0.0, 0.0};

    // RGB curves (4 channels x 256 entries: master, R, G, B). Empty vector
    // means "no curve override" — composite skips curves for this layer.
    QVector<QVector<int>> rgbCurves;

    // White Balance (panel slider values).
    double wbTempSlider = 0.0;
    double wbTintSlider = 0.0;

    // Vignette
    double vigAmount    = 0.0;
    double vigMidpoint  = 0.7;
    double vigRoundness = 0.0;
    double vigFeather   = 0.3;

    QJsonObject toJson() const;
    static AdjustmentLayer fromJson(const QJsonObject &obj);
};

// Cumulative state produced by composing every adjustment layer that covers
// a given timeline time. Field shape mirrors AdjustmentLayer so consumers
// can treat the composite as an "effective layer" without branching.
struct AdjustmentLayerComposite {
    bool gradingEnabled = false;
    double lift[4]  = {0.0, 0.0, 0.0, 0.0};
    double gamma[4] = {0.0, 0.0, 0.0, 0.0};
    double gain[4]  = {0.0, 0.0, 0.0, 0.0};
    QVector<QVector<int>> rgbCurves;   // empty = no curves
    double wbTempSlider = 0.0;
    double wbTintSlider = 0.0;
    double vigAmount    = 0.0;
    double vigMidpoint  = 0.7;
    double vigRoundness = 0.0;
    double vigFeather   = 0.3;
};

// Composition rule (Premiere parity, documented for the consumer):
//
//   * Only layers with `gradingEnabled == true` AND
//     `timelineStartUs <= timelineUs < timelineEndUs` participate.
//   * Layers are evaluated in stack order (lower trackIndex first; ties
//     broken by their order in the input vector — i.e. later-inserted
//     layers on the same track override earlier ones). This matches the
//     "higher track wins" intuition users have from Premiere.
//   * Additive sliders (lift, wbTempSlider, wbTintSlider) accumulate by
//     SUM across the stack. This lets two stacked layers each push lift
//     +5 → composite +10, the natural "more layers, more effect" feel.
//   * Override sliders (gamma, gain, vignette params, rgbCurves) take
//     the TOPMOST non-default value — the highest-priority layer in the
//     stack that explicitly set the value wins. Defaults are detected
//     by comparing against the AdjustmentLayer struct's default values.
//   * Empty layer list → identity composite (gradingEnabled=false, every
//     slider at its default).
AdjustmentLayerComposite composeAdjustmentLayersAt(
    const QVector<AdjustmentLayer> &layers,
    qint64 timelineUs);

// JSON helpers — exposed for ProjectFile round-trip.
QJsonArray adjustmentLayersToJsonArray(const QVector<AdjustmentLayer> &layers);
QVector<AdjustmentLayer> adjustmentLayersFromJsonArray(const QJsonArray &arr);
