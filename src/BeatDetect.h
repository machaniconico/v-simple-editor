#pragma once

// BeatDetect: 音声サンプルからビート(オンセット)時刻と推定 BPM を求める純粋
// エンジン。QApplication 非依存。エネルギーフラックス(半波整流した正の増分)を
// 適応しきい値でピーク採用し、隣接間隔の中央値から BPM を推定する。
// マーカー配線/UI は別ストーリー(BeatDetect 自体は副作用なし)。

#include <QVector>

namespace beatdetect {

struct Config {
    double minBpm      = 60.0;
    double maxBpm      = 200.0;
    double sensitivity = 1.3;   // 適応しきい値の倍率(大きいほど検出を絞る)
    double windowSec   = 0.01;  // オンセット解析フレーム長 (10ms)
};

struct Result {
    QVector<double> beatTimesSec; // 検出ビート時刻(昇順)
    double          bpm = 0.0;    // 推定 BPM (検出不能なら 0)
};

Result detectBeats(const QVector<float>& samples, int sampleRate, const Config& cfg);

} // namespace beatdetect
