#pragma once

#include <QDialog>
#include <QImage>
#include "AutoMatte.h"

class QLabel;
class QSlider;
class QSpinBox;
class QPushButton;
class QTabWidget;

// AutoMatteDialog (AM-3) — automatte エンジン (src/AutoMatte.h) を駆動する UI。
// グリーンバック不要の自動マッティング / 背景除去を試行できる:
//   - クリーンプレート (背景のみフレーム) を読み込めば差分マット、
//     無ければ四隅シードによる autoSegment フォールバックでマットを生成。
//   - しきい値 / erode / dilate / feather / スピル抑制 を調整してプレビュー更新。
//   - 「適用」で applied() を emit し、結果 (合成 or 透過) を呼び出し側へ返す。
// QObject ベース UI のため重い純粋処理はプレビュー縮小サイズで実行する。
class AutoMatteDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AutoMatteDialog(QWidget *parent = nullptr);

    // 元画像 (前景を含むフレーム)。
    void setSourceImage(const QImage &fg);
    // クリーンプレート (背景のみフレーム、任意。無ければ autoSegment フォールバック)。
    void setBackgroundPlate(const QImage &plate);
    // 新背景 (合成先、任意。無ければ透過 PNG 想定で applyMatteAsAlpha)。
    void setNewBackground(const QImage &bg);

    // 現在のパラメータ / 結果アクセス。
    automatte::MatteParams params() const { return m_params; }
    QImage                 resultImage() const { return m_result; }
    QImage                 matteImage() const { return m_matte; }

signals:
    // 「適用」押下時に emit。
    void applied();

private slots:
    void onLoadPlateClicked();
    void onLoadBackgroundClicked();
    void onParamChanged();
    void onApplyClicked();

private:
    void rebuild(bool fullResolution = false); // パラメータからマット / 合成結果を再計算。
    void refreshPreviewInputs();  // 大きな入力画像をプレビュー処理用サイズへキャッシュ。
    void updatePreviews();        // 3 つの QLabel を現在の状態で更新。
    QImage makeCheckerboard(int w, int h, int tileSize = 16) const;

    // Previews (タブ: 元画像 / マット / 合成結果)。
    QTabWidget *m_tabs        = nullptr;
    QLabel     *m_sourceView  = nullptr;
    QLabel     *m_matteView   = nullptr;
    QLabel     *m_resultView  = nullptr;

    // Controls.
    QSlider    *m_thresholdSlider = nullptr;  // 0-100 → threshold 0.0-1.0
    QSpinBox   *m_erodeSpin       = nullptr;
    QSpinBox   *m_dilateSpin      = nullptr;
    QSpinBox   *m_featherSpin     = nullptr;
    QSlider    *m_spillSlider     = nullptr;  // 0-100 → spillSuppress 0.0-1.0
    QPushButton *m_loadPlateBtn   = nullptr;
    QPushButton *m_loadBgBtn      = nullptr;
    QPushButton *m_applyBtn       = nullptr;

    // State.
    QImage                 m_source;   // 元画像 (前景含む)
    QImage                 m_plate;    // クリーンプレート (任意)
    QImage                 m_newBg;    // 合成先背景 (任意)
    QImage                 m_sourcePreview;       // 表示用に縮小済みの元画像
    QImage                 m_previewSourceProcess; // ライブプレビュー処理用に縮小済みの元画像
    QImage                 m_previewPlateProcess;  // m_previewSourceProcess と同サイズ
    QImage                 m_previewBgProcess;     // m_previewSourceProcess と同サイズ
    QImage                 m_matte;    // 生成マット (グレースケール)
    QImage                 m_result;   // 合成 or 透過結果
    automatte::MatteParams m_params;
};
