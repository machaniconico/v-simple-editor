#pragma once

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QList>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

#include <functional>

#include "Deflicker.h"

namespace deflicker {

struct DeflickerDialogContext {
    QString clipLabel;
    QString sourcePath;
    int frameCount = 0;
    int defaultFrame = 0;
    double fps = 30.0;
    QRect maskRegion;
    QSize maskCanvasSize;

    // Preview/analysis fetcher. It may use the active proxy.
    deflicker::FrameFetcher frameFetcher;
    // Apply/export fetcher. When present, this must return original-source
    // frames so generated PNGs never inherit preview resolution.
    deflicker::FrameFetcher sourceFrameFetcher;
    std::function<bool(const QStringList &, double, QString *)> sequenceImporter;
};

} // namespace deflicker

class QLabel;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QSlider;
class QSpinBox;
class DeflickerGraphWidget;
class QCloseEvent;
class QKeyEvent;

class DeflickerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DeflickerDialog(QWidget *parent = nullptr);

    void setContext(const deflicker::DeflickerDialogContext &context);

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onAnalyzeClicked();
    void onApplyClicked();
    void onModeChanged(int index);
    void onUseMaskToggled(bool checked);

private:
    deflicker::DeflickerParams params(const QSize &frameSize = QSize()) const;
    QRect analysisRegion(const QSize &frameSize = QSize()) const;
    QImage fetchFrame(int frameIndex);
    deflicker::FrameFetcher applyFrameFetcher() const;
    QString outputDirectory(QString *error) const;
    void clearAnalysis();
    void setBusy(bool busy);
    void updateModeControls();
    void requestCancel();
    void finishCancelled();

    deflicker::DeflickerDialogContext m_context;
    QHash<int, QImage> m_frameCache;
    QList<int> m_frameCacheOrder;
    QRect m_maskRegion;
    QSize m_maskCanvasSize;

    QLabel *m_clipValue = nullptr;
    QSpinBox *m_startSpin = nullptr;
    QSpinBox *m_endSpin = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QSpinBox *m_windowSpin = nullptr;
    QSlider *m_strengthSlider = nullptr;
    QLabel *m_strengthValue = nullptr;
    QDoubleSpinBox *m_minGainSpin = nullptr;
    QDoubleSpinBox *m_maxGainSpin = nullptr;
    QCheckBox *m_medianCheck = nullptr;
    QSpinBox *m_bandHeightSpin = nullptr;
    QSlider *m_bandSmoothingSlider = nullptr;
    QLabel *m_bandSmoothingValue = nullptr;
    QCheckBox *m_useMaskCheck = nullptr;
    QSpinBox *m_regionXSpin = nullptr;
    QSpinBox *m_regionYSpin = nullptr;
    QSpinBox *m_regionWidthSpin = nullptr;
    QSpinBox *m_regionHeightSpin = nullptr;

    DeflickerGraphWidget *m_graph = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_analyzeButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    bool m_busy = false;
    bool m_cancelRequested = false;
};
