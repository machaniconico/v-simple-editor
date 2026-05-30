#pragma once

// AudioBusPanel — Fairlight 的なオーディオ バス / サブミックスを編集する右側
// ドック (AB-5)。SSOT である audiobus::AudioBusRouting を「所有せず」ポインタで
// 指すだけのビューで、ユーザ操作のたびに routing を直接更新し routingChanged()
// を emit する。MainWindow がそれを受けて AudioMixer::setBusRouting へ反映し、
// プロジェクト保存/読込で永続化する。
//
// UI レイアウト:
//   - 上部にバス一覧 (QListWidget)。各行は「名前 / gain(dB 概算) / M / S」を
//     1 行テキストで表示する。
//   - 選択中バスのストリップ (名前編集 / gain スライダ / ミュート・ソロ ボタン /
//     出力先 QComboBox = master か他バス) を一覧の下に置く。
//   - 最下部に「バス追加」「削除」ボタン。
//
// routing == nullptr のときは全操作が安全に no-op (refresh は一覧を空にする)。

#include <QDockWidget>
#include "AudioBusRouting.h"

class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QSlider;
class QToolButton;
class QComboBox;
class QPushButton;
class QLabel;

class AudioBusPanel : public QDockWidget {
    Q_OBJECT
public:
    explicit AudioBusPanel(QWidget *parent = nullptr);

    // ルーティングモデルを差し込む (所有しない)。nullptr 可。
    void setRouting(audiobus::AudioBusRouting *routing);

    // モデルの現在値で UI を再構築する。外部 (読込等) で routing が書き換わった
    // ときに MainWindow から呼ぶ。
    void refresh();

signals:
    // ユーザ操作で routing を更新したときに発火。MainWindow が AudioMixer へ
    // 反映 + 必要なら再描画する。
    void routingChanged();

private slots:
    void onBusSelectionChanged();
    void onAddBus();
    void onRemoveBus();
    void onRenameBus();
    void onGainSliderChanged(int value);
    void onMuteToggled(bool checked);
    void onSoloToggled(bool checked);
    void onOutputBusChanged(int comboIndex);

private:
    // 現在選択中のバス id (-1 = 未選択)。
    int selectedBusId() const;
    // 出力先 QComboBox を最新のバス一覧で埋める (自分自身は除外)。
    void rebuildOutputCombo(const audiobus::AudioBus *current);
    // 選択ストリップ (右下) を選択バスの値で更新する。
    void updateStrip();
    // gain スライダ値 (0..200, 100=1.0) と線形ゲインの相互変換。
    static double sliderToGain(int value);
    static int gainToSlider(double gain);

    audiobus::AudioBusRouting *m_routing = nullptr;  // 所有しない
    // refresh() / コールバック中に再入信号を抑止するガード。
    bool m_updating = false;

    QListWidget *m_busList = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QSlider *m_gainSlider = nullptr;
    QLabel *m_gainLabel = nullptr;
    QToolButton *m_muteButton = nullptr;
    QToolButton *m_soloButton = nullptr;
    QComboBox *m_outputCombo = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QWidget *m_stripWidget = nullptr;
};
