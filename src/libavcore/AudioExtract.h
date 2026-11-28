#pragma once

#include <QByteArray>
#include <QString>
#include <vector>

namespace libavcore {

// IEEE float32 WAV, interleaved samples; no normalization or quantization.
bool readWavFloatInterleaved(const QString &path, std::vector<float> *samples,
                             int *sampleRate, int *channels, QString *error = nullptr);
bool writeWavFloatInterleaved(const QString &path, const std::vector<float> &samples,
                              int sampleRate, int channels, QString *error = nullptr);

bool readPcm16WavToMono(const QString &wavPath,
                        std::vector<double> &outSamples,
                        int &sampleRate, QString *error = nullptr);

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
