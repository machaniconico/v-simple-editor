#pragma once

#include <QByteArray>
#include <QString>

namespace libavcore {

bool writePcm16AsWav(const QString& wavPath,
                     const QByteArray& pcmS16le,
                     int sampleRate,
                     int channels,
                     QString* error = nullptr);
bool extractAudioToWav(const QString& videoPath,
                       const QString& wavPath,
                       int sampleRate = 16000,
                       QString* error = nullptr);

} // namespace libavcore
