#include "../Keyframe.h"
#include "../VideoEffect.h"
#include "../VideoEffectDialogs.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

#include <QByteArray>
#include <QColor>
#include <QDoubleSpinBox>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtGlobal>

double editKeyframesInitialValue(const ColorCorrection &cc, const KeyframeManager &km,
                                 const QString &prop);

namespace {

constexpr int kClipW = 64;
constexpr int kClipH = 48;
constexpr int kFps = 10;
constexpr int kFrames = 30;
const QSize kOutSize(80, 60);

QStringList g_warnings;
QtMessageHandler g_previousHandler = nullptr;

void captureHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (type == QtWarningMsg)
        g_warnings << msg;
    if (g_previousHandler)
        g_previousHandler(type, ctx, msg);
}

bool writeSourceClip(const QString &path, QString *error)
{
    libavcore::EncodeRequest req;
    req.width = kClipW;
    req.height = kClipH;
    req.fps = kFps;
    req.fpsNum = kFps;
    req.fpsDen = 1;
    req.videoBitrateBits = 600000;
    req.outputPath = path.toStdString();
    req.videoCodecName = "mpeg4";
    req.hwVendorHint = "none";
    req.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (auto err = encoder.open(req)) {
        *error = QString::fromStdString(*err);
        return false;
    }
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < kClipH; ++y)
        for (int x = 0; x < kClipW; ++x)
            frame.setPixelColor(x, y, QColor((32 + x * 3 + y * 5) & 255,
                (64 + x * 7 + y * 2) & 255, (96 + x * 11 + y * 13) & 255));
    for (int i = 0; i < kFrames; ++i) {
        if (!encoder.pushFrame(frame, i)) {
            *error = QStringLiteral("pushFrame failed at frame %1").arg(i);
            return false;
        }
    }
    if (auto err = encoder.finalize()) {
        *error = QString::fromStdString(*err);
        return false;
    }
    return true;
}

ClipInfo makeClip(const QString &path)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = QStringLiteral("legacy-kf");
    clip.duration = clip.outPoint = double(kFrames) / kFps;
    clip.speed = 1.0;
    return clip;
}

void addTrack(ClipInfo &clip, const QString &name, double first, double second)
{
    KeyframeTrack track(name, first);
    track.addKeyframe(0.0, first);
    track.addKeyframe(2.0, second);
    clip.keyframes.addTrack(track);
}

QString projectJson(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = {{clip}};
    return ProjectFile::toJsonString(data);
}

bool loadClip(const QString &json, ClipInfo *out)
{
    ProjectData data;
    if (!ProjectFile::fromJsonString(json, data) || data.videoTracks.isEmpty()
        || data.videoTracks.first().isEmpty())
        return false;
    *out = data.videoTracks.first().first();
    return true;
}

// Second load route: ProjectFile::load (file path) instead of fromJsonString.
bool loadClipFromFile(const QString &json, const QString &path, ClipInfo *out)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json.toUtf8()) != json.toUtf8().size())
        return false;
    file.close();
    ProjectData data;
    if (!ProjectFile::load(path, data) || data.videoTracks.isEmpty()
        || data.videoTracks.first().isEmpty())
        return false;
    *out = data.videoTracks.first().first();
    return true;
}

QImage renderAt(const ClipInfo &clip, double seconds)
{
    Timeline timeline;
    timeline.setProjectOutputConfig(kClipW, kClipH, true);
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}},
        QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    timeline.refreshPlaybackSequence();
    return tlrender::renderFrameAt(&timeline,
        static_cast<qint64>(std::llround(seconds * 1000000.0)), kOutSize);
}

double meanLuma(const QImage &image)
{
    if (image.isNull())
        return -1.0;
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    double sum = 0.0;
    for (int y = 0; y < rgb.height(); ++y)
        for (int x = 0; x < rgb.width(); ++x)
            sum += qGray(rgb.pixel(x, y));
    return sum / (double(rgb.width()) * rgb.height());
}

int maxChannelDiff(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return -1;
    const QImage ca = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage cb = b.convertToFormat(QImage::Format_RGBA8888);
    int worst = 0;
    for (int y = 0; y < ca.height(); ++y) {
        const uchar *ra = ca.constScanLine(y);
        const uchar *rb = cb.constScanLine(y);
        for (int x = 0; x < ca.width() * 4; ++x)
            worst = std::max(worst, std::abs(int(ra[x]) - int(rb[x])));
    }
    return worst;
}

// Project JSON whose nested sequence "seq1" holds `nested` as its V1 clip
// (the sequence store lives in clipParentEntries, not in the main tracks).
QString nestedProjectJson(const ClipInfo &mainClip, const ClipInfo &nested)
{
    QJsonObject root = QJsonDocument::fromJson(projectJson(mainClip).toUtf8()).object();
    const QJsonObject nestedRoot = QJsonDocument::fromJson(projectJson(nested).toUtf8()).object();
    QJsonObject sequence;
    sequence[QStringLiteral("id")] = QStringLiteral("seq1");
    sequence[QStringLiteral("name")] = QStringLiteral("Nested");
    sequence[QStringLiteral("videoTracks")] = nestedRoot.value(QStringLiteral("videoTracks")).toArray();
    sequence[QStringLiteral("audioTracks")] = QJsonArray();
    sequence[QStringLiteral("generatedCaptionOverlays")] = QJsonArray();
    // seq0 is the active (live) sequence; seq1 stays a stored, inactive one.
    QJsonObject active;
    active[QStringLiteral("id")] = QStringLiteral("seq0");
    active[QStringLiteral("name")] = QStringLiteral("Active");
    active[QStringLiteral("videoTracks")] = QJsonArray();
    active[QStringLiteral("audioTracks")] = QJsonArray();
    active[QStringLiteral("generatedCaptionOverlays")] = QJsonArray();
    root[QStringLiteral("sequences")] = QJsonArray{active, sequence};
    root[QStringLiteral("activeSequenceId")] = QStringLiteral("seq0");
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool loadProject(const QString &json, ProjectData *out)
{
    return ProjectFile::fromJsonString(json, *out);
}

bool loadProjectFromFile(const QString &json, const QString &path, ProjectData *out)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json.toUtf8()) != json.toUtf8().size())
        return false;
    file.close();
    return ProjectFile::load(path, *out);
}

bool hasWarningContaining(const QString &needle)
{
    for (const QString &w : g_warnings)
        if (w.contains(needle))
            return true;
    return false;
}

} // namespace

int runLegacyKfSelftest()
{
    selftest::GateTally tally;
    QTemporaryDir temp;
    const QString sourcePath = temp.filePath(QStringLiteral("legacy_kf_src.mp4"));
    QString mediaError;
    QString encoderWhy;
    const bool encoderOk = selftest::requireEncoder(QStringLiteral("mpeg4"), &encoderWhy);
    bool sourceReady = false;
    if (encoderOk && temp.isValid())
        sourceReady = writeSourceClip(sourcePath, &mediaError);
    std::fprintf(stderr, "legacy-kf source: encoder=%d ready=%d %s\n", encoderOk, sourceReady,
        qPrintable(mediaError));

    // G1: a legacy colorCorrection.brightness track loads as grade.brightness
    // and brightens the render between 0 s, 1 s and 2 s.
    if (!encoderOk) {
        tally.skip(1, qPrintable(encoderWhy));
    } else {
        ClipInfo legacy = makeClip(sourcePath);
        addTrack(legacy, QStringLiteral("colorCorrection.brightness"), 0.0, 60.0);
        const QString json = projectJson(legacy);
        // Without migration the legacy-named track is inert: the render must
        // not change over time, proving the luminance gate below is not vacuous.
        const double inert0 = meanLuma(renderAt(legacy, 0.0));
        const double inert2 = meanLuma(renderAt(legacy, 2.0));
        ClipInfo loaded;
        const bool ok = sourceReady && loadClip(json, &loaded);
        const KeyframeTrack *track = ok ? loaded.keyframes.track(QStringLiteral("grade.brightness")) : nullptr;
        const bool keysKept = track && track->count() == 2
            && std::abs(loaded.keyframes.valueAt(QStringLiteral("grade.brightness"), 0.0, -1.0) - 0.0) < 1e-9
            && std::abs(loaded.keyframes.valueAt(QStringLiteral("grade.brightness"), 2.0, -1.0) - 60.0) < 1e-9
            && !loaded.keyframes.hasTrack(QStringLiteral("colorCorrection.brightness"));
        const double l0 = ok ? meanLuma(renderAt(loaded, 0.0)) : -1.0;
        const double l1 = ok ? meanLuma(renderAt(loaded, 1.0)) : -1.0;
        const double l2 = ok ? meanLuma(renderAt(loaded, 2.0)) : -1.0;
        std::fprintf(stderr, "legacy-kf G1 loaded=%d keysKept=%d luma@0=%.3f luma@1=%.3f luma@2=%.3f "
            "(want strictly increasing, steps>=0.5) inertLegacy@0=%.3f inertLegacy@2=%.3f (want equal)\n",
            ok, keysKept, l0, l1, l2, inert0, inert2);
        tally.gate(1, sourceReady && ok && keysKept && l1 - l0 >= 0.5 && l2 - l1 >= 0.5
            && std::abs(inert2 - inert0) < 1e-9, "brightness migration");
    }

    // G2: hue / gamma / highlights / shadows legacy tracks are renamed and the
    // hue key renders like static hue 0 / 60 / 120.
    if (!encoderOk) {
        tally.skip(2, qPrintable(encoderWhy));
    } else {
        ClipInfo legacy = makeClip(sourcePath);
        addTrack(legacy, QStringLiteral("colorCorrection.hue"), 0.0, 120.0);
        addTrack(legacy, QStringLiteral("colorCorrection.gamma"), 1.0, 1.0);
        addTrack(legacy, QStringLiteral("colorCorrection.highlights"), 0.0, 0.0);
        addTrack(legacy, QStringLiteral("colorCorrection.shadows"), 0.0, 0.0);
        ClipInfo loaded;
        const bool ok = sourceReady && loadClip(projectJson(legacy), &loaded);
        bool renamed = ok;
        for (const char *n : {"hue", "gamma", "highlights", "shadows"}) {
            renamed = renamed && loaded.keyframes.hasTrack(QStringLiteral("grade.") + QLatin1String(n))
                && !loaded.keyframes.hasTrack(QStringLiteral("colorCorrection.") + QLatin1String(n));
        }
        const double staticHue[3] = {0.0, 60.0, 120.0};
        int diff[3] = {-1, -1, -1};
        int vsDefault = -1;
        if (ok) {
            for (int i = 0; i < 3; ++i) {
                ClipInfo reference = makeClip(sourcePath);
                reference.colorCorrection.hue = staticHue[i];
                diff[i] = maxChannelDiff(renderAt(loaded, double(i)), renderAt(reference, double(i)));
            }
            vsDefault = maxChannelDiff(renderAt(loaded, 1.0), renderAt(makeClip(sourcePath), 1.0));
        }
        std::fprintf(stderr, "legacy-kf G2 renamed=%d diff@0s=%d diff@1s=%d diff@2s=%d (want <=1) "
            "diffVsStaticHue0@1s=%d (want >=3)\n", renamed, diff[0], diff[1], diff[2], vsDefault);
        tally.gate(2, sourceReady && renamed && diff[0] >= 0 && diff[0] <= 1
            && diff[1] >= 0 && diff[1] <= 1 && diff[2] >= 0 && diff[2] <= 1 && vsDefault >= 3,
            "hue/gamma/highlights/shadows migration");
    }

    // G3: a project without legacy tracks reads and writes byte-identically.
    {
        ClipInfo plain = makeClip(QStringLiteral("legacy_kf_plain.mp4"));
        plain.colorCorrection.brightness = 7.0;
        addTrack(plain, QStringLiteral("grade.contrast"), -10.0, 10.0);
        const QString before = projectJson(plain);
        ClipInfo viaString, viaFile;
        const bool a = loadClip(before, &viaString);
        const bool b = loadClipFromFile(before, temp.filePath(QStringLiteral("plain.veditor")), &viaFile);
        const bool same = a && b && projectJson(viaString) == before && projectJson(viaFile) == before;
        std::fprintf(stderr, "legacy-kf G3 fromJsonString=%d load=%d identical=%d\n", a, b, same);
        tally.gate(3, same, "no-legacy roundtrip");
    }

    // G4: after migration (both load routes) the saved JSON has no colorCorrection. names.
    {
        ClipInfo legacy = makeClip(QStringLiteral("legacy_kf_g4.mp4"));
        for (const char *n : {"brightness", "contrast", "saturation", "exposure", "hue",
                              "temperature", "gamma", "highlights", "shadows"})
            addTrack(legacy, QStringLiteral("colorCorrection.") + QLatin1String(n), 0.0, 1.0);
        const QString json = projectJson(legacy);
        ClipInfo viaString, viaFile;
        const bool a = loadClip(json, &viaString);
        const bool b = loadClipFromFile(json, temp.filePath(QStringLiteral("legacy.veditor")), &viaFile);
        const QString savedA = a ? projectJson(viaString) : QString();
        const QString savedB = b ? projectJson(viaFile) : QString();
        int gradeTracks = 0;
        for (const char *n : {"brightness", "contrast", "saturation", "exposure", "hue",
                              "temperature", "gamma", "highlights", "shadows"})
            gradeTracks += viaString.keyframes.hasTrack(QStringLiteral("grade.") + QLatin1String(n))
                && viaFile.keyframes.hasTrack(QStringLiteral("grade.") + QLatin1String(n));
        const bool inputHadLegacy = json.contains(QStringLiteral("colorCorrection."));
        std::fprintf(stderr, "legacy-kf G4 inputHadLegacy=%d string=%d file=%d gradeTracks=%d/9 "
            "legacyInStringSave=%d legacyInFileSave=%d\n", inputHadLegacy, a, b, gradeTracks,
            savedA.contains(QStringLiteral("colorCorrection.")),
            savedB.contains(QStringLiteral("colorCorrection.")));
        tally.gate(4, inputHadLegacy && a && b && gradeTracks == 9
            && !savedA.contains(QStringLiteral("colorCorrection."))
            && !savedB.contains(QStringLiteral("colorCorrection.")), "saved JSON legacy-free");
    }

    // G4 (nested): legacy tracks inside the nested sequence store migrate on both
    // load routes; the saved JSON (sequences array included) is legacy-free.
    {
        ClipInfo nested = makeClip(QStringLiteral("legacy_kf_nested.mp4"));
        addTrack(nested, QStringLiteral("colorCorrection.brightness"), 0.0, 60.0);
        addTrack(nested, QStringLiteral("colorCorrection.hue"), 0.0, 120.0);
        const QString json = nestedProjectJson(makeClip(QStringLiteral("legacy_kf_main.mp4")), nested);
        ProjectData viaString, viaFile;
        const bool a = loadProject(json, &viaString);
        const bool b = loadProjectFromFile(json, temp.filePath(QStringLiteral("nested.veditor")), &viaFile);
        const QString savedA = a ? ProjectFile::toJsonString(viaString) : QString();
        const QString savedB = b ? ProjectFile::toJsonString(viaFile) : QString();
        const bool sequencesSaved = savedA.contains(QStringLiteral("seq1"))
            && savedB.contains(QStringLiteral("seq1"));
        const bool gradeSaved = savedA.contains(QStringLiteral("grade.brightness"))
            && savedA.contains(QStringLiteral("grade.hue"))
            && savedB.contains(QStringLiteral("grade.brightness"))
            && savedB.contains(QStringLiteral("grade.hue"));
        std::fprintf(stderr, "legacy-kf G4n inputHadLegacy=%d string=%d file=%d sequencesSaved=%d "
            "gradeSaved=%d legacyInStringSave=%d legacyInFileSave=%d\n",
            json.contains(QStringLiteral("colorCorrection.")), a, b, sequencesSaved, gradeSaved,
            savedA.contains(QStringLiteral("colorCorrection.")),
            savedB.contains(QStringLiteral("colorCorrection.")));
        tally.gate(7, json.contains(QStringLiteral("colorCorrection.")) && a && b && sequencesSaved
            && gradeSaved && !savedA.contains(QStringLiteral("colorCorrection."))
            && !savedB.contains(QStringLiteral("colorCorrection.")), "nested sequence migration");
    }

    // G8: the migrated nested sequence clip renders brighter over time; a
    // grade/legacy duplicate inside the nested store warns.
    if (!encoderOk) {
        tally.skip(8, qPrintable(encoderWhy));
    } else {
        ClipInfo nested = makeClip(sourcePath);
        addTrack(nested, QStringLiteral("colorCorrection.brightness"), 0.0, 60.0);
        ProjectData data;
        const bool loaded = sourceReady && loadProject(
            nestedProjectJson(makeClip(sourcePath), nested), &data);
        QHash<QString, QString> entries;
        for (const ClipParentEntry &e : data.clipParentEntries)
            entries.insert(e.clipId, e.parentClipId);
        Timeline timeline;
        timeline.setClipParentEntries(entries);
        TimelineSequence nestedSeq;
        for (const TimelineSequence &seq : timeline.sequences())
            if (seq.id == QLatin1String("seq1"))
                nestedSeq = seq;
        const bool haveClip = loaded && !nestedSeq.videoTracks.isEmpty()
            && !nestedSeq.videoTracks.first().isEmpty();
        double l0 = -1.0, l1 = -1.0, l2 = -1.0;
        bool hasGrade = false;
        // The raw store blob (not the Timeline's re-expanded copy) must carry grade.brightness.
        bool blobHasGrade = false;
        for (const ClipParentEntry &e : data.clipParentEntries)
            blobHasGrade = blobHasGrade || (QByteArray::fromBase64(e.parentClipId.toLatin1())
                .contains("grade.brightness") && !QByteArray::fromBase64(e.parentClipId.toLatin1())
                .contains("colorCorrection."));
        if (haveClip) {
            const ClipInfo &nc = nestedSeq.videoTracks.first().first();
            hasGrade = nc.keyframes.hasTrack(QStringLiteral("grade.brightness"));
            l0 = meanLuma(renderAt(nc, 0.0));
            l1 = meanLuma(renderAt(nc, 1.0));
            l2 = meanLuma(renderAt(nc, 2.0));
        }
        ClipInfo dup = makeClip(QStringLiteral("legacy_kf_nested_dup.mp4"));
        addTrack(dup, QStringLiteral("grade.brightness"), 10.0, 20.0);
        addTrack(dup, QStringLiteral("colorCorrection.brightness"), 90.0, 95.0);
        g_previousHandler = qInstallMessageHandler(captureHandler);
        g_warnings.clear();
        ProjectData dupData;
        const bool dupLoaded = loadProject(
            nestedProjectJson(makeClip(QStringLiteral("legacy_kf_main.mp4")), dup), &dupData);
        const bool dupWarned = hasWarningContaining(QStringLiteral("colorCorrection.brightness"));
        qInstallMessageHandler(g_previousHandler);
        g_previousHandler = nullptr;
        std::fprintf(stderr, "legacy-kf G8 loaded=%d haveClip=%d hasGrade=%d blobHasGradeOnly=%d "
            "luma@0=%.3f luma@1=%.3f luma@2=%.3f (want strictly increasing) dupLoaded=%d dupWarned=%d\n",
            loaded, haveClip, hasGrade, blobHasGrade, l0, l1, l2, dupLoaded, dupWarned);
        tally.gate(8, haveClip && hasGrade && blobHasGrade && l1 - l0 >= 0.5 && l2 - l1 >= 0.5
            && dupLoaded && dupWarned, "nested sequence render + warning");
    }

    // G5: grade.* and colorCorrection.* both present -> grade wins, warning emitted
    // (both load routes).
    {
        ClipInfo both = makeClip(QStringLiteral("legacy_kf_g5.mp4"));
        addTrack(both, QStringLiteral("grade.brightness"), 10.0, 20.0);
        addTrack(both, QStringLiteral("colorCorrection.brightness"), 90.0, 95.0);
        const QString json = projectJson(both);
        g_previousHandler = qInstallMessageHandler(captureHandler);
        g_warnings.clear();
        ClipInfo viaString;
        const bool a = loadClip(json, &viaString);
        const bool warnedString = hasWarningContaining(QStringLiteral("colorCorrection.brightness"));
        g_warnings.clear();
        ClipInfo viaFile;
        const bool b = loadClipFromFile(json, temp.filePath(QStringLiteral("both.veditor")), &viaFile);
        const bool warnedFile = hasWarningContaining(QStringLiteral("colorCorrection.brightness"));
        qInstallMessageHandler(g_previousHandler);
        g_previousHandler = nullptr;
        const auto gradeValue = [](const ClipInfo &c, double t) {
            return c.keyframes.valueAt(QStringLiteral("grade.brightness"), t, -1.0);
        };
        const bool valuesKept = a && b
            && std::abs(gradeValue(viaString, 0.0) - 10.0) < 1e-9 && std::abs(gradeValue(viaString, 2.0) - 20.0) < 1e-9
            && std::abs(gradeValue(viaFile, 0.0) - 10.0) < 1e-9 && std::abs(gradeValue(viaFile, 2.0) - 20.0) < 1e-9
            && viaString.keyframes.track(QStringLiteral("grade.brightness"))->count() == 2
            && !viaString.keyframes.hasTrack(QStringLiteral("colorCorrection.brightness"))
            && !viaFile.keyframes.hasTrack(QStringLiteral("colorCorrection.brightness"));
        std::fprintf(stderr, "legacy-kf G5 string: loaded=%d grade@0=%g grade@2=%g warned=%d; "
            "file: loaded=%d grade@0=%g grade@2=%g warned=%d\n", a, gradeValue(viaString, 0.0),
            gradeValue(viaString, 2.0), warnedString, b, gradeValue(viaFile, 0.0),
            gradeValue(viaFile, 2.0), warnedFile);
        tally.gate(5, valuesKept && warnedString && warnedFile, "grade wins + warning");
    }

    // G6: the "Edit Keyframes" dialog's initial value for a new track is the
    // clip's static value for all 9 items; an existing track uses its value at 0 s.
    {
        ColorCorrection cc;
        cc.brightness = 11.0; cc.contrast = 12.0; cc.saturation = 13.0; cc.exposure = 0.5;
        cc.hue = 14.0; cc.temperature = 15.0; cc.gamma = 1.7; cc.highlights = 16.0; cc.shadows = -17.0;
        struct Case { const char *prop; double want; };
        const Case cases[] = {{"grade.brightness", 11.0}, {"grade.contrast", 12.0},
            {"grade.saturation", 13.0}, {"grade.exposure", 0.5}, {"grade.hue", 14.0},
            {"grade.temperature", 15.0}, {"grade.gamma", 1.7}, {"grade.highlights", 16.0},
            {"grade.shadows", -17.0}};
        bool all = true;
        QStringList details;
        for (const Case &c : cases) {
            const double got = editKeyframesInitialValue(cc, KeyframeManager{}, QString::fromLatin1(c.prop));
            all = all && std::abs(got - c.want) < 1e-9;
            details << QStringLiteral("%1=%2").arg(QLatin1String(c.prop)).arg(got);
        }
        const double defaultGamma = editKeyframesInitialValue(ColorCorrection{}, KeyframeManager{},
            QStringLiteral("grade.gamma"));
        KeyframeManager existing;
        KeyframeTrack gammaTrack(QStringLiteral("grade.gamma"), 1.0);
        gammaTrack.addKeyframe(0.0, 2.5);
        gammaTrack.addKeyframe(2.0, 0.5);
        existing.addTrack(gammaTrack);
        const double fromTrack = editKeyframesInitialValue(cc, existing, QStringLiteral("grade.gamma"));
        // Dialog built (not exec'd) the way editKeyframes builds it.
        const double staticGamma = 1.7;
        KeyframeDialog newDialog(KeyframeTrack(QStringLiteral("grade.gamma"), staticGamma), 3.0);
        const QDoubleSpinBox *newSpin = newDialog.findChild<QDoubleSpinBox *>(QStringLiteral("keyframeValueSpin"));
        KeyframeDialog existingDialog(gammaTrack, 3.0);
        const QDoubleSpinBox *existingSpin = existingDialog.findChild<QDoubleSpinBox *>(QStringLiteral("keyframeValueSpin"));
        const double newSpinValue = newSpin ? newSpin->value() : -999.0;
        const double existingSpinValue = existingSpin ? existingSpin->value() : -999.0;
        std::fprintf(stderr, "legacy-kf G6 %s defaultGamma=%g (want 1) existingTrack@0s=%g (want 2.5) "
            "dialogSpin(new, static gamma 1.7)=%g dialogSpin(existing)=%g (want 2.5)\n",
            qPrintable(details.join(QLatin1Char(' '))), defaultGamma, fromTrack,
            newSpinValue, existingSpinValue);
        tally.gate(6, all && std::abs(defaultGamma - 1.0) < 1e-9 && std::abs(fromTrack - 2.5) < 1e-9
            && std::abs(newSpinValue - staticGamma) < 1e-9 && std::abs(existingSpinValue - 2.5) < 1e-9,
            "dialog initial values");
    }

    return tally.finish("legacy-kf");
}
