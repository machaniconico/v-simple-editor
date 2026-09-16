#include "../ExportDialog.h"
#include "../RenderQueue.h"
#include "../libavcore/Encode.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QTemporaryDir>
#include <cstdio>

namespace {
QMap<QString, QString> options(const libavcore::EncodeRequest& request,
                              const QString& encoder)
{
    QMap<QString, QString> result;
    for (const auto& entry : libavcore::codecOptionsFor(request, encoder))
        result.insert(entry.first, entry.second);
    return result;
}
}

int runExportProSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    using Request = libavcore::EncodeRequest;
    Request request;
    request.videoBitrateBits = 10000000;
    // Literal values from pre-US-409 configureEncoderContext, including its
    // AVCodecContext bit_rate assignment; no legacy algorithm is reproduced.
    const QMap<QString, QString> x26x{
        {"bit_rate", "10000000"}, {"preset", "medium"}, {"crf", "23"}};
    const QMap<QString, QString> av1{
        {"bit_rate", "10000000"}, {"preset", "8"}, {"crf", "30"}};
    const QMap<QString, QString> nvenc{
        {"bit_rate", "10000000"}, {"preset", "p4"}, {"rc", "vbr"}, {"cq", "23"}};
    request.crf = 7; // Bitrate mode must ignore this opt-in setting.
    gate(1, options(request, "libx264") == x26x
         && options(request, "libx265") == x26x
         && options(request, "libsvtav1") == av1
         && options(request, "h264_nvenc") == nvenc);

    request.rateControl = Request::RateControl::Crf;
    request.crf = 20;
    bool quality = true;
    for (const QString& encoder : {QString("libx264"), QString("libx265"),
                                  QString("libsvtav1"), QString("h264_nvenc")}) {
        const auto actual = options(request, encoder);
        quality &= !actual.contains("bit_rate")
            && actual.value(encoder.contains("nvenc") ? "cq" : "crf") == "20";
    }
    gate(2, quality);

    request.crf = 60;
    bool clamped = options(request, "libx264").value("crf") == "51"
        && options(request, "libx265").value("crf") == "51"
        && options(request, "libsvtav1").value("crf") == "60";
    request.crf = -5;
    clamped &= options(request, "libsvtav1").value("crf") == "0";
    request.crf = 100;
    clamped &= options(request, "libsvtav1").value("crf") == "63";
    request.crf = -1;
    clamped &= options(request, "libx264").value("crf") == "23"
        && options(request, "libsvtav1").value("crf") == "30"
        && options(request, "h264_nvenc").value("cq") == "23";
    gate(3, clamped);

    QTemporaryDir directory;
    RenderQueue queue;
    RenderJob legacy;
    legacy.uuid = "legacy";
    queue.addJob(legacy);
    RenderJob defaults;
    defaults.uuid = "defaults";
    defaults.exportConfig = {{"rateControl", "bitrate"}, {"crf", -1}};
    queue.addJob(defaults);
    RenderJob custom;
    custom.uuid = "quality";
    custom.exportConfig = {{"rateControl", "crf"}, {"crf", 20}};
    queue.addJob(custom);
    const QString path = directory.filePath("queue.json");
    bool roundtrip = directory.isValid() && queue.saveQueue(path);
    QFile file(path);
    roundtrip &= file.open(QIODevice::ReadOnly);
    const auto jobs = QJsonDocument::fromJson(file.readAll()).object().value("jobs").toArray();
    file.close();
    roundtrip &= jobs.size() == 3;
    for (int i = 0; i < 2 && i < jobs.size(); ++i) {
        const auto config = jobs.at(i).toObject().value("exportConfig").toObject();
        roundtrip &= !config.contains("rateControl") && !config.contains("crf");
    }
    RenderQueue restored;
    roundtrip &= restored.loadQueue(path);
    const auto restoredJobs = restored.jobs();
    roundtrip &= restoredJobs.size() == 3;
    if (restoredJobs.size() == 3) {
        roundtrip &= restoredJobs.at(0).exportConfig.value("rateControl").toString("bitrate") == "bitrate"
            && restoredJobs.at(0).exportConfig.value("crf").toInt(-1) == -1
            && restoredJobs.at(2).exportConfig.value("rateControl").toString() == "crf"
            && restoredJobs.at(2).exportConfig.value("crf").toInt() == 20;
    }
    const ExportConfig config;
    roundtrip &= config.rateControl == ExportConfig::RateControl::Bitrate && config.crf == -1;
    gate(4, roundtrip);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
