#include "SubtitleGenerator.h"
#include "WhisperTranscriber.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QTextStream>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

SubtitleGenerator::SubtitleGenerator(QObject *parent)
    : QObject(parent) {}

// --- Public API ---

void SubtitleGenerator::generate(const QString &videoFilePath, const WhisperConfig &config)
{
    // Extract audio to temp WAV, then run whisper
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        emit errorOccurred("Failed to create temporary directory");
        return;
    }
    tmpDir.setAutoRemove(false);
    m_tempDir = tmpDir.path();

    QString wavPath = m_tempDir + "/audio.wav";
    emit progressChanged(5);

    if (!extractAudio(videoFilePath, wavPath)) {
        emit errorOccurred("Failed to extract audio from video");
        QDir(m_tempDir).removeRecursively();
        return;
    }

    emit progressChanged(30);
    runWhisper(wavPath, config);
}

void SubtitleGenerator::generateFromAudio(const QString &audioFilePath, const WhisperConfig &config)
{
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        emit errorOccurred("Failed to create temporary directory");
        return;
    }
    tmpDir.setAutoRemove(false);
    m_tempDir = tmpDir.path();

    emit progressChanged(10);
    runWhisper(audioFilePath, config);
}

// --- Audio extraction via FFmpeg API ---

bool SubtitleGenerator::extractAudio(const QString &videoPath, const QString &outputWavPath)
{
    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, videoPath.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    // Find audio stream
    int audioIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioIdx = static_cast<int>(i);
            break;
        }
    }
    if (audioIdx < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    auto *codecpar = fmtCtx->streams[audioIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVCodecContext *decCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(decCtx, codecpar);
    if (avcodec_open2(decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    // Resample to 16kHz mono s16 (whisper's required format)
    constexpr int outSampleRate = 16000;
    SwrContext *swrCtx = nullptr;
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MONO;
    swr_alloc_set_opts2(&swrCtx,
        &outLayout, AV_SAMPLE_FMT_S16, outSampleRate,
        &decCtx->ch_layout, decCtx->sample_fmt, decCtx->sample_rate,
        0, nullptr);

    if (!swrCtx || swr_init(swrCtx) < 0) {
        if (swrCtx) swr_free(&swrCtx);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    // Open output WAV file
    QFile outFile(outputWavPath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        swr_free(&swrCtx);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    // Write WAV header placeholder (44 bytes)
    QByteArray header(44, '\0');
    outFile.write(header);

    // Decode and resample all audio
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    qint64 totalSamples = 0;

    auto decodeFrame = [&]() {
        int outSamples = swr_get_out_samples(swrCtx, frame->nb_samples);
        QVector<int16_t> buf(outSamples);
        uint8_t *outBuf = reinterpret_cast<uint8_t*>(buf.data());
        int converted = swr_convert(swrCtx, &outBuf, outSamples,
            const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        if (converted > 0) {
            outFile.write(reinterpret_cast<const char*>(buf.data()),
                          converted * static_cast<int>(sizeof(int16_t)));
            totalSamples += converted;
        }
    };

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != audioIdx) {
            av_packet_unref(packet);
            continue;
        }
        if (avcodec_send_packet(decCtx, packet) == 0) {
            while (avcodec_receive_frame(decCtx, frame) == 0)
                decodeFrame();
        }
        av_packet_unref(packet);
    }

    // Flush decoder
    avcodec_send_packet(decCtx, nullptr);
    while (avcodec_receive_frame(decCtx, frame) == 0)
        decodeFrame();

    // Flush resampler
    {
        QVector<int16_t> buf(4096);
        uint8_t *outBuf = reinterpret_cast<uint8_t*>(buf.data());
        int converted = swr_convert(swrCtx, &outBuf, 4096, nullptr, 0);
        if (converted > 0) {
            outFile.write(reinterpret_cast<const char*>(buf.data()),
                          converted * static_cast<int>(sizeof(int16_t)));
            totalSamples += converted;
        }
    }

    // Write proper WAV header
    qint64 dataSize = totalSamples * sizeof(int16_t);
    outFile.seek(0);

    auto writeLE16 = [&](uint16_t v) { outFile.write(reinterpret_cast<const char*>(&v), 2); };
    auto writeLE32 = [&](uint32_t v) { outFile.write(reinterpret_cast<const char*>(&v), 4); };

    outFile.write("RIFF", 4);
    writeLE32(static_cast<uint32_t>(36 + dataSize));
    outFile.write("WAVE", 4);
    outFile.write("fmt ", 4);
    writeLE32(16);                                          // chunk size
    writeLE16(1);                                           // PCM format
    writeLE16(1);                                           // mono
    writeLE32(static_cast<uint32_t>(outSampleRate));        // sample rate
    writeLE32(static_cast<uint32_t>(outSampleRate * 2));    // byte rate
    writeLE16(2);                                           // block align
    writeLE16(16);                                          // bits per sample
    outFile.write("data", 4);
    writeLE32(static_cast<uint32_t>(dataSize));

    outFile.close();

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swrCtx);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);

    return totalSamples > 0;
}

// --- Whisper integration ---

void SubtitleGenerator::runWhisper(const QString &audioPath, const WhisperConfig &config)
{
    QString whisperBin = findWhisperBinary();
    if (whisperBin.isEmpty()) {
        emit errorOccurred("Whisper is not installed. Install openai-whisper or whisper.cpp.");
        QDir(m_tempDir).removeRecursively();
        return;
    }

    bool isWhisperCpp = whisperBin.contains("main")
        || whisperBin.contains("whisper-cpp")
        || whisperBin.contains("whisper-cli");

    QStringList args;
    if (isWhisperCpp) {
        // whisper.cpp main binary
        args << "-f" << audioPath;
        args << "--output-json";
        if (config.wordTimestamps || whisperBin.contains("whisper-cli"))
            args << "--output-json-full";
        args << "-of" << (m_tempDir + "/result");
        if (!config.modelPath.isEmpty())
            args << "-m" << config.modelPath;
        if (config.language != "auto")
            args << "-l" << config.language;
        if (config.translateToEnglish)
            args << "--translate";
    } else {
        // openai-whisper (Python)
        args << audioPath;
        args << "--output_format" << "json";
        args << "--output_dir" << m_tempDir;
        if (!config.modelPath.isEmpty())
            args << "--model" << config.modelPath;
        if (config.language != "auto")
            args << "--language" << config.language;
        if (config.translateToEnglish)
            args << "--task" << "translate";
        if (config.wordTimestamps)
            args << "--word_timestamps" << "True";
    }

    m_process = new QProcess(this);

    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        QString output = m_process->readAllStandardError();
        // Parse progress from whisper stderr (e.g., percentage indicators)
        static QRegularExpression re(R"((\d+)%)");
        auto match = re.match(output);
        if (match.hasMatch()) {
            int pct = match.captured(1).toInt();
            // Scale: 30-90% of total progress is whisper
            emit progressChanged(30 + pct * 60 / 100);
        }
    });

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, isWhisperCpp, audioPath](int exitCode, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || exitCode != 0) {
            QString err = m_process->readAllStandardError();
            emit errorOccurred(QString("Whisper failed (exit %1): %2").arg(exitCode).arg(err));
            QDir(m_tempDir).removeRecursively();
            m_process->deleteLater();
            m_process = nullptr;
            return;
        }

        emit progressChanged(90);

        // Find JSON output file
        QString jsonPath;
        if (isWhisperCpp) {
            jsonPath = m_tempDir + "/result.json";
        } else {
            // openai-whisper names output after the input file
            QString baseName = QFileInfo(audioPath).completeBaseName();
            jsonPath = m_tempDir + "/" + baseName + ".json";
        }

        QVector<SubtitleSegment> segments = parseWhisperOutput(jsonPath);
        emit progressChanged(100);
        emit generationComplete(segments);

        QDir(m_tempDir).removeRecursively();
        m_process->deleteLater();
        m_process = nullptr;
    });

    m_process->start(whisperBin, args);
}

QString SubtitleGenerator::findWhisperBinary()
{
    // Check for openai-whisper (Python package)
    QString whisperPath = QStandardPaths::findExecutable("whisper");
    if (!whisperPath.isEmpty())
        return whisperPath;

    // Check for whisper.cpp main binary
    QString mainPath = QStandardPaths::findExecutable("whisper-cpp");
    if (!mainPath.isEmpty())
        return mainPath;

    mainPath = QStandardPaths::findExecutable("whisper-cli");
    if (!mainPath.isEmpty())
        return mainPath;

    mainPath = QStandardPaths::findExecutable("main", {"/usr/local/bin", "/opt/homebrew/bin"});
    if (!mainPath.isEmpty()) {
        // Verify it's actually whisper.cpp by checking --help
        QProcess test;
        test.start(mainPath, {"--help"});
        test.waitForFinished(3000);
        if (test.readAllStandardOutput().contains("whisper"))
            return mainPath;
    }

    return {};
}

bool SubtitleGenerator::isWhisperAvailable()
{
    return !findWhisperBinary().isEmpty();
}

QStringList SubtitleGenerator::availableModels()
{
    QStringList models;
    QString whisperBin = findWhisperBinary();
    if (whisperBin.isEmpty())
        return models;

    bool isWhisperCpp = whisperBin.contains("main")
        || whisperBin.contains("whisper-cpp")
        || whisperBin.contains("whisper-cli");

    if (!isWhisperCpp) {
        // openai-whisper standard model names
        models << "tiny" << "tiny.en"
               << "base" << "base.en"
               << "small" << "small.en"
               << "medium" << "medium.en"
               << "large" << "large-v2" << "large-v3";
    } else {
        // whisper.cpp: scan for .bin model files in common locations
        QStringList searchPaths = {
            QDir::homePath() + "/.cache/whisper",
            QDir::homePath() + "/whisper.cpp/models",
            "/usr/local/share/whisper/models",
            "/opt/homebrew/share/whisper/models"
        };
        for (const auto &dir : searchPaths) {
            QDir d(dir);
            if (!d.exists()) continue;
            for (const auto &entry : d.entryList({"ggml-*.bin"}, QDir::Files)) {
                QString name = entry;
                name.remove("ggml-").remove(".bin");
                if (!models.contains(name))
                    models.append(name);
            }
        }
        if (models.isEmpty())
            models << "base" << "small" << "medium" << "large";
    }

    return models;
}

// --- JSON parsing ---

QVector<SubtitleSegment> SubtitleGenerator::parseWhisperOutput(const QString &jsonPath)
{
    QVector<SubtitleSegment> segments;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly))
        return segments;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject())
        return segments;

    QJsonObject root = doc.object();
    if (!root.value(QStringLiteral("segments")).isArray()
        && root.value(QStringLiteral("transcription")).isArray()) {
        QString lang;
        QString error;
        const QList<speech::Segment> speechSegments =
            whisper::WhisperTranscriber::parseWhisperJsonSegments(doc.toJson(QJsonDocument::Compact), &lang, &error);
        if (error.isEmpty())
            return whisper::WhisperTranscriber::toSubtitleSegments(speechSegments, lang);
    }

    // Both openai-whisper and whisper.cpp use "segments" array
    QJsonArray segs = root["segments"].toArray();
    QString lang = root["language"].toString();
    for (const QJsonValue &val : segs) {
        const QJsonObject obj = val.toObject();
        if (obj.value(QStringLiteral("tokens")).isArray()) {
            QString detectedLang = lang;
            QString error;
            const QList<speech::Segment> speechSegments =
                whisper::WhisperTranscriber::parseWhisperJsonSegments(doc.toJson(QJsonDocument::Compact), &detectedLang, &error);
            if (error.isEmpty())
                return whisper::WhisperTranscriber::toSubtitleSegments(speechSegments, detectedLang);
            break;
        }
    }

    for (const auto &val : segs) {
        QJsonObject obj = val.toObject();
        SubtitleSegment seg;
        seg.startTime = obj["start"].toDouble();
        seg.endTime = obj["end"].toDouble();
        seg.text = obj["text"].toString().trimmed();
        seg.language = lang;

        // whisper.cpp uses "confidence", openai-whisper uses "avg_logprob"
        if (obj.contains("confidence")) {
            seg.confidence = obj["confidence"].toDouble();
        } else if (obj.contains("avg_logprob")) {
            // Convert log probability to rough 0-1 confidence
            double logprob = obj["avg_logprob"].toDouble();
            seg.confidence = qBound(0.0, std::exp(logprob), 1.0);
        }

        if (obj.contains("words") && obj["words"].isArray()) {
            QJsonArray words = obj["words"].toArray();
            for (const QJsonValue &wordVal : words) {
                QJsonObject wordObj = wordVal.toObject();
                const bool hasText = wordObj.contains("word") || wordObj.contains("text");
                const bool hasStart = wordObj.contains("start") || wordObj.contains("t0");
                const bool hasEnd = wordObj.contains("end") || wordObj.contains("t1");
                if (!hasText || !hasStart || !hasEnd)
                    continue;

                SubtitleWord word;
                word.text = wordObj.contains("word")
                    ? wordObj["word"].toString().trimmed()
                    : wordObj["text"].toString().trimmed();
                if (word.text.isEmpty())
                    continue;

                word.startTime = wordObj.contains("start")
                    ? wordObj["start"].toDouble()
                    : wordObj["t0"].toDouble();
                word.endTime = wordObj.contains("end")
                    ? wordObj["end"].toDouble()
                    : wordObj["t1"].toDouble();
                seg.words.append(word);
            }
        }

        if (!seg.text.isEmpty())
            segments.append(seg);
    }

    return segments;
}

// --- SRT export ---

bool SubtitleGenerator::exportSRT(const QVector<SubtitleSegment> &segments, const QString &outputPath)
{
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    for (int i = 0; i < segments.size(); ++i) {
        const auto &seg = segments[i];
        out << (i + 1) << "\n";
        out << formatTimeSRT(seg.startTime) << " --> " << formatTimeSRT(seg.endTime) << "\n";
        out << seg.text << "\n";
        out << "\n";
    }

    file.close();
    return true;
}

// --- WebVTT export ---

bool SubtitleGenerator::exportVTT(const QVector<SubtitleSegment> &segments, const QString &outputPath)
{
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    out << "WEBVTT\n\n";

    for (int i = 0; i < segments.size(); ++i) {
        const auto &seg = segments[i];
        out << (i + 1) << "\n";
        out << formatTimeVTT(seg.startTime) << " --> " << formatTimeVTT(seg.endTime) << "\n";
        out << seg.text << "\n";
        out << "\n";
    }

    file.close();
    return true;
}

// --- Convert to text overlays ---

QVector<EnhancedTextOverlay> SubtitleGenerator::toTextOverlays(const QVector<SubtitleSegment> &segments)
{
    QVector<EnhancedTextOverlay> overlays;
    overlays.reserve(segments.size());

    for (const auto &seg : segments) {
        EnhancedTextOverlay overlay;
        overlay.text = seg.text;
        overlay.startTime = seg.startTime;
        overlay.endTime = seg.endTime;

        // Subtitle styling defaults
        overlay.font = QFont("Arial", 28, QFont::Bold);
        overlay.color = Qt::white;
        overlay.backgroundColor = QColor(0, 0, 0, 160);
        overlay.outlineColor = Qt::black;
        overlay.outlineWidth = 2;

        // Position at bottom center
        overlay.x = 0.5;
        overlay.y = 0.9;
        overlay.alignment = Qt::AlignCenter;
        overlay.wordWrap = true;
        overlay.visible = true;

        overlays.append(overlay);
    }

    return overlays;
}

// --- Time formatting ---

QString SubtitleGenerator::formatTimeSRT(double seconds)
{
    // SRT format: HH:MM:SS,mmm
    int totalMs = static_cast<int>(std::round(seconds * 1000.0));
    int h = totalMs / 3600000;
    int m = (totalMs % 3600000) / 60000;
    int s = (totalMs % 60000) / 1000;
    int ms = totalMs % 1000;
    return QString("%1:%2:%3,%4")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}

QString SubtitleGenerator::formatTimeVTT(double seconds)
{
    // VTT format: HH:MM:SS.mmm
    int totalMs = static_cast<int>(std::round(seconds * 1000.0));
    int h = totalMs / 3600000;
    int m = (totalMs % 3600000) / 60000;
    int s = (totalMs % 60000) / 1000;
    int ms = totalMs % 1000;
    return QString("%1:%2:%3.%4")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}
