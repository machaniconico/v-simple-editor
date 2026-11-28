#include "../MainWindow.h"
#include "../AudioMixer.h"
#include "../Exporter.h"
#include "../ProjectFile.h"
#include "../RenderQueue.h"
#include "../RenderQueueDialog.h"
#include "../VideoPlayer.h"

#include <QApplication>
#include <QDialog>
#include <QJsonDocument>
#include <QTimer>
#include <atomic>
#include <cstdio>

namespace {
// Explicit-instantiation access keeps the production headers unchanged. Read
// real mixer/queue state, not the persistence property's mirrored value.
template<class Tag, typename Tag::Type Member>
struct Access {
    friend typename Tag::Type member(Tag) { return Member; }
};
#define ACCESS(Name, TypeExpr, MemberExpr) \
    struct Name { using Type = TypeExpr; friend Type member(Name); }; \
    template struct Access<Name, MemberExpr>;
ACCESS(Collect, void (MainWindow::*)(ProjectData &), &MainWindow::populateProjectData)
ACCESS(Load, void (MainWindow::*)(const ProjectData &, const QString &), &MainWindow::applyLoadedProjectData)
ACCESS(Normalize, void (MainWindow::*)(double, double), &MainWindow::applyLoudnessNormalize)
ACCESS(NewProject, void (MainWindow::*)(), &MainWindow::newProject)
ACCESS(Player, VideoPlayer * MainWindow::*, &MainWindow::m_player)
ACCESS(Queue, RenderQueue * MainWindow::*, &MainWindow::m_renderQueue)
ACCESS(QueueDialog, RenderQueueDialog * MainWindow::*, &MainWindow::m_renderQueueDialog)
ACCESS(Export, Exporter * MainWindow::*, &MainWindow::m_exporter)
ACCESS(ExportGain, double Exporter::*, &Exporter::m_loudnessGainDb)
ACCESS(QueueGain, double RenderQueue::*, &RenderQueue::m_loudnessGainDb)
ACCESS(Amount, std::atomic<double> AudioMixer::*, &AudioMixer::m_normalizerAmount)
ACCESS(Uniformity, std::atomic<double> AudioMixer::*, &AudioMixer::m_normalizerUniformity)
#undef ACCESS
}

int runLoudnessPersistSelftest()
{
    int passed = 0, failed = 0;
    const auto check = [&](bool ok, const char *gate) {
        std::fprintf(stderr, "[loudness-persist] %s %s\n", ok ? "PASS" : "FAIL", gate);
        ok ? ++passed : ++failed;
    };
    MainWindow window;
    if (!(window.*member(Queue{})))
        window.*member(Queue{}) = new RenderQueue(&window);
    if (!(window.*member(QueueDialog{})))
        window.*member(QueueDialog{}) = new RenderQueueDialog(&window);
    auto *mixer = (window.*member(Player{}))->audioMixer();
    const auto collect = [&] {
        ProjectData data;
        (window.*member(Collect{}))(data);
        return data;
    };
    const auto load = [&](const ProjectData &data) {
        (window.*member(Load{}))(data, QString());
    };
    const auto normalize = [&](double gain) {
        (window.*member(Normalize{}))(-14.0, gain);
    };
    const auto gainsEqual = [&](double gain) {
        return (window.*member(Export{}))->*member(ExportGain{}) == gain
            && (window.*member(Queue{}))->*member(QueueGain{}) == gain
            && (window.*member(QueueDialog{}))->queue()->*member(QueueGain{}) == gain;
    };
    const auto amountEqual = [&](double amount) {
        return mixer && (mixer->*member(Amount{})).load() == amount
            && (mixer->*member(Uniformity{})).load() == 0.5;
    };
    const auto reset = [&] {
        const auto data = collect();
        return gainsEqual(0.0) && amountEqual(0.0)
            && data.loudnessSettings.value("loudnessGainDb").toDouble(-1.0) == 0.0
            && data.loudnessSettings.value("normalizerAmount").toDouble(-1.0) == 0.0;
    };

    normalize(2.25);
    const ProjectData saved = collect();
    ProjectData restored;
    const bool roundTrip = ProjectFile::fromJsonString(ProjectFile::toJsonString(saved), restored);
    normalize(-6.0);
    load(restored);
    check(roundTrip && saved.loudnessSettings.value("loudnessGainDb").toDouble() == 2.25
              && gainsEqual(2.25)
              && RenderQueue::buildLoudnessAudioFilterArgs(
                     (window.*member(Queue{}))->*member(QueueGain{}))
                  == QStringList({QStringLiteral("-af"),
                       QStringLiteral("volume=2.25dB,alimiter=limit=0.98")}), "G1");

    const bool normalizedRestored = amountEqual(1.0)
        && saved.loudnessSettings.value("normalizerAmount").toDouble() == 1.0;
    // A fractional amount must survive independently of gain (not inferred
    // as gain != 0); exercise another round trip through the real collector.
    restored.loudnessSettings["normalizerAmount"] = 0.35;
    load(restored);
    const ProjectData fractional = collect();
    ProjectData fractionalRestored;
    const bool fractionalRoundTrip = ProjectFile::fromJsonString(
        ProjectFile::toJsonString(fractional), fractionalRestored);
    normalize(0.0);
    load(fractionalRestored);
    check(normalizedRestored && fractionalRoundTrip && gainsEqual(2.25)
              && amountEqual(0.35), "G2");

    load(ProjectData{}); // Opening another/legacy project with no settings.
    const bool otherReset = reset();
    normalize(4.0);
    bool acceptedNewProject = false;
    QTimer::singleShot(0, &window, [&] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
            acceptedNewProject = true;
            dialog->accept();
        }
    });
    (window.*member(NewProject{}))();
    check(otherReset && acceptedNewProject && reset(), "G3");

    // Literal schema fixture of the legacy unused loudness block. This is
    // serialized data, not a second implementation of the old collector.
    const QJsonObject legacy = QJsonDocument::fromJson(
        R"({"normalizerAmount":0,"loudnessGainDb":0,"_timecodeBurnIn":{
            "enabled":false,"position":"bottomCenter","fontSizePct":4,
            "showFrames":true,"dropFrame":false,"prefix":"",
            "showClipName":false,"showDate":false,"opacity":0.8}})").object();
    const QJsonObject unused = collect().loudnessSettings;
    ProjectData legacyProject;
    legacyProject.loudnessSettings = legacy;
    ProjectData legacyRestored;
    const bool legacyRoundTrip = ProjectFile::fromJsonString(
        ProjectFile::toJsonString(legacyProject), legacyRestored);
    normalize(3.0);
    load(legacyRestored);
    check(legacyRoundTrip && unused == legacy
              && QJsonDocument(unused).toJson(QJsonDocument::Compact)
                  == QJsonDocument(legacy).toJson(QJsonDocument::Compact)
              && reset() && collect().loudnessSettings == legacy, "G4");

    std::fprintf(stderr, "[loudness-persist] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
