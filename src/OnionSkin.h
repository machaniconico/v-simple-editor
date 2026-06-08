#pragma once
// OnionSkin: 前後フレームを半透明で重ねるプレビュー表示専用オーバーレイ。
// display-local 適用のみ。export / render / timeline 経路には一切関与しない。

#include <QImage>
#include <QVector>

namespace onionskin {

struct Config {
    bool enabled = false;
    int framesBefore = 1;
    int framesAfter = 1;
    double opacity = 0.35;
    bool tintColors = true;
};

QImage compose(const QImage &current,
               const QVector<QImage> &before,
               const QVector<QImage> &after,
               const Config &cfg);

} // namespace onionskin
