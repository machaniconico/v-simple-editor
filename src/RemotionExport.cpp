#include "RemotionExport.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QTextStream>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>
#include <QtMath>
#include <algorithm>

// ---------------------------------------------------------------------------
// RemotionExporter
// ---------------------------------------------------------------------------

RemotionExporter::RemotionExporter(QObject *parent)
    : QObject(parent)
{
}

bool RemotionExporter::exportProject(const RemotionExportConfig &config, const ProjectData &data)
{
    if (config.outputDir.isEmpty()) {
        emit exportError("Output directory is not set.");
        return false;
    }
    if (config.projectName.isEmpty()) {
        emit exportError("Project name is not set.");
        return false;
    }

    const QString base    = config.outputDir + "/" + config.projectName;
    const QString srcDir  = base + "/src";
    const QString compDir = srcDir + "/components";
    const QString libDir  = srcDir + "/lib";
    const QString assetDir = srcDir + "/assets";

    emit exportProgress(0);

    // 1. Directory structure
    if (!createDirectoryStructure(base, config.projectName)) return false;
    emit exportProgress(5);

    // 2. Root config files
    if (config.generatePackageJson) {
        if (!writePackageJson(base, config)) return false;
    }
    if (!writeTsConfig(base)) return false;
    emit exportProgress(15);

    // 3. src/Root.tsx
    if (!writeRootTsx(srcDir, config)) return false;
    emit exportProgress(25);

    // 4. src/Video.tsx
    if (!writeVideoTsx(srcDir, config, data)) return false;
    emit exportProgress(40);

    // 5. src/lib/timeline.ts
    if (!writeTimelineTs(libDir, config, data)) return false;
    emit exportProgress(50);

    // 6. Components
    if (!writeVideoClipTsx(compDir))    return false;
    if (!writeAudioClipTsx(compDir))    return false;
    if (!writeTextOverlayTsx(compDir))  return false;
    if (!writeImageOverlayTsx(compDir)) return false;
    if (!writeTransitionTsx(compDir))   return false;
    if (!writeEffectsTsx(compDir))      return false;
    emit exportProgress(75);

    // 7. Assets
    if (config.includeAssets) {
        if (!copyAssets(assetDir, data)) return false;
    }
    emit exportProgress(95);

    emit exportProgress(100);
    emit exportComplete(base);
    return true;
}

// ---------------------------------------------------------------------------
// Directory helpers
// ---------------------------------------------------------------------------

bool RemotionExporter::createDirectoryStructure(const QString &base,
                                                 const QString & /*projectName*/)
{
    const QStringList dirs = {
        base,
        base + "/src",
        base + "/src/components",
        base + "/src/lib",
        base + "/src/assets",
    };
    for (const QString &dir : dirs) {
        if (!QDir().mkpath(dir)) {
            emit exportError(QString("Failed to create directory: %1").arg(dir));
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// writeFile
// ---------------------------------------------------------------------------

bool RemotionExporter::writeFile(const QString &path, const QString &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit exportError(QString("Cannot write file: %1").arg(path));
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << content;
    return true;
}

// ---------------------------------------------------------------------------
// package.json
// ---------------------------------------------------------------------------

bool RemotionExporter::writePackageJson(const QString &base,
                                         const RemotionExportConfig &config)
{
    const QString ver = config.remotionVersion;
    QString content = QString(
        "{\n"
        "  \"name\": \"%1\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"description\": \"Remotion project exported from V Editor\",\n"
        "  \"scripts\": {\n"
        "    \"start\": \"npx remotion preview\",\n"
        "    \"build\": \"npx remotion render\",\n"
        "    \"render\": \"npx remotion render src/index.ts VideoComposition out/video.mp4\"\n"
        "  },\n"
        "  \"dependencies\": {\n"
        "    \"@remotion/cli\": \"%2.x\",\n"
        "    \"remotion\": \"%3.x\",\n"
        "    \"react\": \"^18.0.0\",\n"
        "    \"react-dom\": \"^18.0.0\"\n"
        "  },\n"
        "  \"devDependencies\": {\n"
        "    \"@types/react\": \"^18.0.0\",\n"
        "    \"@types/react-dom\": \"^18.0.0\",\n"
        "    \"typescript\": \"^5.0.0\"\n"
        "  }\n"
        "}\n"
    ).arg(config.projectName, ver, ver);
    return writeFile(base + "/package.json", content);
}

// ---------------------------------------------------------------------------
// tsconfig.json
// ---------------------------------------------------------------------------

bool RemotionExporter::writeTsConfig(const QString &base)
{
    const QString content =
        "{\n"
        "  \"compilerOptions\": {\n"
        "    \"lib\": [\"dom\", \"esnext\"],\n"
        "    \"module\": \"commonjs\",\n"
        "    \"target\": \"ES2022\",\n"
        "    \"strict\": true,\n"
        "    \"outDir\": \"./dist\",\n"
        "    \"rootDir\": \"./src\",\n"
        "    \"jsx\": \"react\",\n"
        "    \"allowSyntheticDefaultImports\": true,\n"
        "    \"esModuleInterop\": true,\n"
        "    \"moduleResolution\": \"node\",\n"
        "    \"resolveJsonModule\": true\n"
        "  },\n"
        "  \"include\": [\"src\"]\n"
        "}\n";
    return writeFile(base + "/tsconfig.json", content);
}

// ---------------------------------------------------------------------------
// src/Root.tsx
// ---------------------------------------------------------------------------

bool RemotionExporter::writeRootTsx(const QString &srcDir,
                                     const RemotionExportConfig &config)
{
    const int dur = (config.durationInFrames > 0)
                  ? config.durationInFrames
                  : 300; // placeholder; real value comes from timeline.ts

    const QString content = QString(
        "import { Composition } from 'remotion';\n"
        "import { VideoComposition } from './Video';\n"
        "\n"
        "export const RemotionRoot: React.FC = () => {\n"
        "  return (\n"
        "    <>\n"
        "      <Composition\n"
        "        id=\"VideoComposition\"\n"
        "        component={VideoComposition}\n"
        "        durationInFrames={%1}\n"
        "        fps={%2}\n"
        "        width={%3}\n"
        "        height={%4}\n"
        "      />\n"
        "    </>\n"
        "  );\n"
        "};\n"
    ).arg(dur).arg(config.fps).arg(config.width).arg(config.height);
    return writeFile(srcDir + "/Root.tsx", content);
}

// ---------------------------------------------------------------------------
// Duration helper
// ---------------------------------------------------------------------------

int RemotionExporter::calculateDurationInFrames(const ProjectData &data, int fps) const
{
    double maxDuration = 0.0;

    auto processTrack = [&](const QVector<QVector<ClipInfo>> &tracks) {
        for (const auto &track : tracks) {
            double trackDur = 0.0;
            for (const auto &clip : track)
                trackDur += clip.effectiveDuration();
            maxDuration = std::max(maxDuration, trackDur);
        }
    };

    processTrack(data.videoTracks);
    processTrack(data.audioTracks);

    if (maxDuration <= 0.0) maxDuration = 10.0;
    return static_cast<int>(std::ceil(maxDuration * fps));
}

// ---------------------------------------------------------------------------
// src/Video.tsx
// ---------------------------------------------------------------------------

bool RemotionExporter::writeVideoTsx(const QString &srcDir,
                                      const RemotionExportConfig &config,
                                      const ProjectData &data)
{
    const QString body = generateVideoCompositionBody(config, data);

    const QString content = QString(
        "import React from 'react';\n"
        "import {\n"
        "  AbsoluteFill,\n"
        "  Sequence,\n"
        "  useCurrentFrame,\n"
        "  useVideoConfig,\n"
        "  interpolate,\n"
        "} from 'remotion';\n"
        "import { VideoClip } from './components/VideoClip';\n"
        "import { AudioClip } from './components/AudioClip';\n"
        "import { TextOverlay } from './components/TextOverlay';\n"
        "import { ImageOverlay } from './components/ImageOverlay';\n"
        "import { Transition } from './components/Transition';\n"
        "import { TIMELINE } from './lib/timeline';\n"
        "\n"
        "export const VideoComposition: React.FC = () => {\n"
        "  const frame = useCurrentFrame();\n"
        "  const { fps, width, height, durationInFrames } = useVideoConfig();\n"
        "\n"
        "%1"
        "};\n"
    ).arg(body);
    return writeFile(srcDir + "/Video.tsx", content);
}

QString RemotionExporter::generateVideoCompositionBody(const RemotionExportConfig &config,
                                                        const ProjectData &data) const
{
    const int fps = config.fps;
    QString out;
    QTextStream s(&out);

    s << "  return (\n";
    s << "    <AbsoluteFill style={{ backgroundColor: '#000' }}>\n";

    // Video tracks — each track is layered via AbsoluteFill
    int videoTrackIdx = 0;
    for (const auto &track : data.videoTracks) {
        double offset = 0.0;
        int clipIdx = 0;
        for (const auto &clip : track) {
            const int startFrame  = static_cast<int>(std::round(offset * fps));
            const int durFrames   = static_cast<int>(std::ceil(clip.effectiveDuration() * fps));
            const int startFrom   = static_cast<int>(std::round(clip.inPoint * fps));
            const int endAt       = (clip.outPoint > 0.0)
                                  ? static_cast<int>(std::round(clip.outPoint * fps))
                                  : 0;

            QFileInfo fi(clip.filePath);
            const QString assetSrc = config.includeAssets
                ? QString("./assets/%1").arg(fi.fileName())
                : clip.filePath;

            // CSS filter from color correction + effects
            QString filterStr;
            if (!clip.colorCorrection.isDefault()) {
                filterStr += colorCorrectionToCSS(clip.colorCorrection);
            }
            for (const auto &fx : clip.effects) {
                if (fx.enabled) filterStr += effectTypeToCSS(fx);
            }

            s << "      {/* Video Track " << videoTrackIdx << " Clip " << clipIdx << " */}\n";
            s << "      <AbsoluteFill>\n";
            s << "        <Sequence\n";
            s << "          from={" << startFrame << "}\n";
            s << "          durationInFrames={" << durFrames << "}\n";
            s << "          name=\"" << fi.baseName() << "\"\n";
            s << "        >\n";
            s << "          <VideoClip\n";
            s << "            src={\"" << assetSrc << "\"}\n";
            s << "            startFrom={" << startFrom << "}\n";
            if (endAt > 0)
                s << "            endAt={" << endAt << "}\n";
            if (clip.speed != 1.0)
                s << "            playbackRate={" << clip.speed << "}\n";
            if (clip.volume != 1.0)
                s << "            volume={" << clip.volume << "}\n";
            if (!filterStr.isEmpty())
                s << "            style={{ filter: \"" << filterStr.trimmed() << "\" }}\n";
            s << "          />\n";

            // Text overlays inside this clip's Sequence
            const auto &overlays = clip.textManager.overlays();
            for (const auto &ov : overlays) {
                if (!ov.visible) continue;
                const int ovStart = static_cast<int>(std::round(ov.startTime * fps));
                const int ovEnd   = (ov.endTime > 0.0)
                                  ? static_cast<int>(std::round(ov.endTime * fps))
                                  : durFrames;
                const int ovDur   = ovEnd - ovStart;
                s << "          <Sequence from={" << ovStart << "} durationInFrames={" << ovDur << "}>\n";
                s << "            <TextOverlay\n";
                s << "              text={" << "\"" << QString(ov.text).replace("\"", "\\\"") << "\"}\n";
                s << "              x={" << ov.x << "}\n";
                s << "              y={" << ov.y << "}\n";
                s << "              opacity={" << ov.opacity << "}\n";
                s << "              rotation={" << ov.rotation << "}\n";
                s << "              scale={" << ov.scale << "}\n";
                s << "              animIn={\"" << TextAnimation::typeName(ov.animIn.type) << "\"}\n";
                s << "              animOut={\"" << TextAnimation::typeName(ov.animOut.type) << "\"}\n";
                s << "              animDuration={" << ov.animIn.duration << "}\n";
                s << "            />\n";
                s << "          </Sequence>\n";
            }

            s << "        </Sequence>\n";
            s << "      </AbsoluteFill>\n";

            offset += clip.effectiveDuration();
            ++clipIdx;
        }
        ++videoTrackIdx;
    }

    // Audio tracks
    int audioTrackIdx = 0;
    for (const auto &track : data.audioTracks) {
        double offset = 0.0;
        int clipIdx = 0;
        for (const auto &clip : track) {
            const int startFrame  = static_cast<int>(std::round(offset * fps));
            const int durFrames   = static_cast<int>(std::ceil(clip.effectiveDuration() * fps));
            const int startFrom   = static_cast<int>(std::round(clip.inPoint * fps));
            const int endAt       = (clip.outPoint > 0.0)
                                  ? static_cast<int>(std::round(clip.outPoint * fps))
                                  : 0;

            QFileInfo fi(clip.filePath);
            const QString assetSrc = config.includeAssets
                ? QString("./assets/%1").arg(fi.fileName())
                : clip.filePath;

            s << "      {/* Audio Track " << audioTrackIdx << " Clip " << clipIdx << " */}\n";
            s << "      <Sequence\n";
            s << "        from={" << startFrame << "}\n";
            s << "        durationInFrames={" << durFrames << "}\n";
            s << "        name=\"audio-" << audioTrackIdx << "-" << clipIdx << "\"\n";
            s << "        layout=\"none\"\n";
            s << "      >\n";
            s << "        <AudioClip\n";
            s << "          src={\"" << assetSrc << "\"}\n";
            s << "          startFrom={" << startFrom << "}\n";
            if (endAt > 0)
                s << "          endAt={" << endAt << "}\n";
            if (clip.speed != 1.0)
                s << "          playbackRate={" << clip.speed << "}\n";
            s << "          volume={" << clip.volume << "}\n";
            s << "        />\n";
            s << "      </Sequence>\n";

            offset += clip.effectiveDuration();
            ++clipIdx;
        }
        ++audioTrackIdx;
    }

    s << "    </AbsoluteFill>\n";
    s << "  );\n";

    return out;
}

// ---------------------------------------------------------------------------
// Color correction → CSS filter string
// ---------------------------------------------------------------------------

QString RemotionExporter::colorCorrectionToCSS(const ColorCorrection &cc) const
{
    QString f;

    // brightness: editor -100..100 → CSS 0..2 (1=normal)
    if (cc.brightness != 0.0) {
        double v = 1.0 + cc.brightness / 100.0;
        f += QString("brightness(%1) ").arg(v, 0, 'f', 3);
    }
    // contrast: -100..100 → CSS 0..2
    if (cc.contrast != 0.0) {
        double v = 1.0 + cc.contrast / 100.0;
        f += QString("contrast(%1) ").arg(v, 0, 'f', 3);
    }
    // saturation: -100..100 → CSS 0..2
    if (cc.saturation != 0.0) {
        double v = 1.0 + cc.saturation / 100.0;
        f += QString("saturate(%1) ").arg(v, 0, 'f', 3);
    }
    // hue: degrees
    if (cc.hue != 0.0) {
        f += QString("hue-rotate(%1deg) ").arg(cc.hue, 0, 'f', 1);
    }
    // exposure: treated as additional brightness
    if (cc.exposure != 0.0) {
        double v = std::pow(2.0, cc.exposure);
        f += QString("brightness(%1) ").arg(v, 0, 'f', 3);
    }

    return f;
}

// ---------------------------------------------------------------------------
// VideoEffect → CSS filter
// ---------------------------------------------------------------------------

QString RemotionExporter::effectTypeToCSS(const VideoEffect &effect) const
{
    switch (effect.type) {
    case VideoEffectType::Blur:
        return QString("blur(%1px) ").arg(effect.param1, 0, 'f', 1);
    case VideoEffectType::Sepia:
        return QString("sepia(%1) ").arg(effect.param1, 0, 'f', 2);
    case VideoEffectType::Grayscale:
        return "grayscale(1) ";
    case VideoEffectType::Invert:
        return "invert(1) ";
    default:
        return {};
    }
}

// ---------------------------------------------------------------------------
// Keyframe interpolation name
// ---------------------------------------------------------------------------

QString RemotionExporter::interpolationName(KeyframePoint::Interpolation interp) const
{
    switch (interp) {
    case KeyframePoint::EaseIn:    return "easeIn";
    case KeyframePoint::EaseOut:   return "easeOut";
    case KeyframePoint::EaseInOut: return "easeInOut";
    case KeyframePoint::Hold:      return "hold";
    default:                       return "linear";
    }
}

// ---------------------------------------------------------------------------
// Text animation → Remotion interpolate code snippet
// ---------------------------------------------------------------------------

QString RemotionExporter::animationTypeToRemotionCode(TextAnimationType type,
                                                       const QString &progressVar) const
{
    switch (type) {
    case TextAnimationType::FadeIn:
        return QString("opacity: interpolate(%1, [0, 1], [0, 1])").arg(progressVar);
    case TextAnimationType::FadeOut:
        return QString("opacity: interpolate(%1, [0, 1], [1, 0])").arg(progressVar);
    case TextAnimationType::FadeInOut:
        return QString("opacity: interpolate(%1, [0, 0.5, 1], [0, 1, 0])").arg(progressVar);
    case TextAnimationType::SlideLeft:
        return QString("transform: `translateX(${interpolate(%1, [0, 1], [100, 0])}px)`").arg(progressVar);
    case TextAnimationType::SlideRight:
        return QString("transform: `translateX(${interpolate(%1, [0, 1], [-100, 0])}px)`").arg(progressVar);
    case TextAnimationType::SlideUp:
        return QString("transform: `translateY(${interpolate(%1, [0, 1], [50, 0])}px)`").arg(progressVar);
    case TextAnimationType::SlideDown:
        return QString("transform: `translateY(${interpolate(%1, [0, 1], [-50, 0])}px)`").arg(progressVar);
    case TextAnimationType::ScaleIn:
        return QString("transform: `scale(${interpolate(%1, [0, 1], [0, 1])})`").arg(progressVar);
    case TextAnimationType::Pop:
        return QString("transform: `scale(${interpolate(%1, [0, 0.7, 1], [0, 1.2, 1])})`").arg(progressVar);
    default:
        return {};
    }
}

// ---------------------------------------------------------------------------
// src/lib/timeline.ts
// ---------------------------------------------------------------------------

bool RemotionExporter::writeTimelineTs(const QString &libDir,
                                        const RemotionExportConfig &config,
                                        const ProjectData &data)
{
    const QString body = generateTimelineData(config, data);
    const QString content =
        "// Auto-generated by V Editor — do not edit manually\n\n"
        + body;
    return writeFile(libDir + "/timeline.ts", content);
}

QString RemotionExporter::generateTimelineData(const RemotionExportConfig &config,
                                                const ProjectData &data) const
{
    const int fps = config.fps;
    const int totalFrames = (config.durationInFrames > 0)
                          ? config.durationInFrames
                          : calculateDurationInFrames(data, fps);

    QString out;
    QTextStream s(&out);

    s << "export const FPS = " << fps << ";\n";
    s << "export const WIDTH = " << config.width << ";\n";
    s << "export const HEIGHT = " << config.height << ";\n";
    s << "export const DURATION_IN_FRAMES = " << totalFrames << ";\n\n";

    // ClipData type
    s << "export interface ClipData {\n";
    s << "  src: string;\n";
    s << "  startFrame: number;\n";
    s << "  durationInFrames: number;\n";
    s << "  startFrom: number;\n";
    s << "  endAt?: number;\n";
    s << "  playbackRate?: number;\n";
    s << "  volume?: number;\n";
    s << "  filter?: string;\n";
    s << "}\n\n";

    // TextData type
    s << "export interface TextData {\n";
    s << "  text: string;\n";
    s << "  startFrame: number;\n";
    s << "  durationInFrames: number;\n";
    s << "  x: number;\n";
    s << "  y: number;\n";
    s << "  opacity: number;\n";
    s << "  rotation: number;\n";
    s << "  scale: number;\n";
    s << "  animIn: string;\n";
    s << "  animOut: string;\n";
    s << "  animDuration: number;\n";
    s << "}\n\n";

    // Video track data
    s << "export const TIMELINE = {\n";
    s << "  videoTracks: [\n";

    for (int ti = 0; ti < data.videoTracks.size(); ++ti) {
        const auto &track = data.videoTracks[ti];
        s << "    // Track " << ti << "\n";
        s << "    [\n";

        double offset = 0.0;
        for (const auto &clip : track) {
            const int startFrame = static_cast<int>(std::round(offset * fps));
            const int durFrames  = static_cast<int>(std::ceil(clip.effectiveDuration() * fps));
            const int startFrom  = static_cast<int>(std::round(clip.inPoint * fps));
            const int endAt      = (clip.outPoint > 0.0)
                                 ? static_cast<int>(std::round(clip.outPoint * fps))
                                 : 0;

            QFileInfo fi(clip.filePath);
            const QString src = config.includeAssets
                ? QString("./assets/%1").arg(fi.fileName())
                : clip.filePath;

            QString filterStr;
            if (!clip.colorCorrection.isDefault())
                filterStr += colorCorrectionToCSS(clip.colorCorrection);
            for (const auto &fx : clip.effects)
                if (fx.enabled) filterStr += effectTypeToCSS(fx);

            s << "      {\n";
            s << "        src: \"" << src << "\",\n";
            s << "        startFrame: " << startFrame << ",\n";
            s << "        durationInFrames: " << durFrames << ",\n";
            s << "        startFrom: " << startFrom << ",\n";
            if (endAt > 0) s << "        endAt: " << endAt << ",\n";
            if (clip.speed != 1.0)
                s << "        playbackRate: " << clip.speed << ",\n";
            if (clip.volume != 1.0)
                s << "        volume: " << clip.volume << ",\n";
            if (!filterStr.isEmpty())
                s << "        filter: \"" << filterStr.trimmed() << "\",\n";

            // Keyframe tracks
            if (clip.keyframes.hasAnyKeyframes()) {
                s << "        keyframes: {\n";
                for (const auto &track2 : clip.keyframes.tracks()) {
                    if (!track2.hasKeyframes()) continue;
                    s << "          " << track2.propertyName() << ": [\n";
                    for (const auto &kf : track2.keyframes()) {
                        s << "            { frame: " << static_cast<int>(std::round(kf.time * fps))
                          << ", value: " << kf.value
                          << ", easing: \"" << interpolationName(kf.interpolation) << "\" },\n";
                    }
                    s << "          ],\n";
                }
                s << "        },\n";
            }

            // Text overlays
            const auto &overlays = clip.textManager.overlays();
            if (!overlays.isEmpty()) {
                s << "        textOverlays: [\n";
                for (const auto &ov : overlays) {
                    if (!ov.visible) continue;
                    const int ovStart  = static_cast<int>(std::round(ov.startTime * fps));
                    const int ovEnd    = (ov.endTime > 0.0)
                                      ? static_cast<int>(std::round(ov.endTime * fps))
                                      : durFrames;
                    s << "          {\n";
                    s << "            text: \"" << QString(ov.text).replace("\"", "\\\"") << "\",\n";
                    s << "            startFrame: " << ovStart << ",\n";
                    s << "            durationInFrames: " << (ovEnd - ovStart) << ",\n";
                    s << "            x: " << ov.x << ",\n";
                    s << "            y: " << ov.y << ",\n";
                    s << "            opacity: " << ov.opacity << ",\n";
                    s << "            rotation: " << ov.rotation << ",\n";
                    s << "            scale: " << ov.scale << ",\n";
                    s << "            animIn: \"" << TextAnimation::typeName(ov.animIn.type) << "\",\n";
                    s << "            animOut: \"" << TextAnimation::typeName(ov.animOut.type) << "\",\n";
                    s << "            animDuration: " << ov.animIn.duration << ",\n";
                    s << "          },\n";
                }
                s << "        ],\n";
            }

            s << "      },\n";
            offset += clip.effectiveDuration();
        }

        s << "    ],\n";
    }

    s << "  ],\n";
    s << "  audioTracks: [\n";

    for (int ti = 0; ti < data.audioTracks.size(); ++ti) {
        const auto &track = data.audioTracks[ti];
        s << "    // Track " << ti << "\n";
        s << "    [\n";
        double offset = 0.0;
        for (const auto &clip : track) {
            const int startFrame = static_cast<int>(std::round(offset * fps));
            const int durFrames  = static_cast<int>(std::ceil(clip.effectiveDuration() * fps));
            const int startFrom  = static_cast<int>(std::round(clip.inPoint * fps));
            const int endAt      = (clip.outPoint > 0.0)
                                 ? static_cast<int>(std::round(clip.outPoint * fps))
                                 : 0;

            QFileInfo fi(clip.filePath);
            const QString src = config.includeAssets
                ? QString("./assets/%1").arg(fi.fileName())
                : clip.filePath;

            s << "      {\n";
            s << "        src: \"" << src << "\",\n";
            s << "        startFrame: " << startFrame << ",\n";
            s << "        durationInFrames: " << durFrames << ",\n";
            s << "        startFrom: " << startFrom << ",\n";
            if (endAt > 0) s << "        endAt: " << endAt << ",\n";
            if (clip.speed != 1.0)
                s << "        playbackRate: " << clip.speed << ",\n";
            s << "        volume: " << clip.volume << ",\n";
            s << "      },\n";

            offset += clip.effectiveDuration();
        }
        s << "    ],\n";
    }

    s << "  ],\n";
    s << "} as const;\n";

    return out;
}

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

bool RemotionExporter::writeVideoClipTsx(const QString &compDir)
{
    const QString content =
        "import React from 'react';\n"
        "import { Video, OffthreadVideo } from 'remotion';\n"
        "\n"
        "interface VideoClipProps {\n"
        "  src: string;\n"
        "  startFrom?: number;\n"
        "  endAt?: number;\n"
        "  playbackRate?: number;\n"
        "  volume?: number;\n"
        "  style?: React.CSSProperties;\n"
        "  offthread?: boolean;\n"
        "}\n"
        "\n"
        "export const VideoClip: React.FC<VideoClipProps> = ({\n"
        "  src,\n"
        "  startFrom = 0,\n"
        "  endAt,\n"
        "  playbackRate = 1,\n"
        "  volume = 1,\n"
        "  style,\n"
        "  offthread = false,\n"
        "}) => {\n"
        "  const commonProps = {\n"
        "    src,\n"
        "    startFrom,\n"
        "    ...(endAt !== undefined ? { endAt } : {}),\n"
        "    playbackRate,\n"
        "    volume,\n"
        "    style: { width: '100%', height: '100%', objectFit: 'cover' as const, ...style },\n"
        "  };\n"
        "\n"
        "  if (offthread) {\n"
        "    return <OffthreadVideo {...commonProps} />;\n"
        "  }\n"
        "  return <Video {...commonProps} />;\n"
        "};\n";
    return writeFile(compDir + "/VideoClip.tsx", content);
}

bool RemotionExporter::writeAudioClipTsx(const QString &compDir)
{
    const QString content =
        "import React from 'react';\n"
        "import { Audio } from 'remotion';\n"
        "\n"
        "interface AudioClipProps {\n"
        "  src: string;\n"
        "  startFrom?: number;\n"
        "  endAt?: number;\n"
        "  playbackRate?: number;\n"
        "  volume?: number;\n"
        "}\n"
        "\n"
        "export const AudioClip: React.FC<AudioClipProps> = ({\n"
        "  src,\n"
        "  startFrom = 0,\n"
        "  endAt,\n"
        "  playbackRate = 1,\n"
        "  volume = 1,\n"
        "}) => {\n"
        "  return (\n"
        "    <Audio\n"
        "      src={src}\n"
        "      startFrom={startFrom}\n"
        "      {...(endAt !== undefined ? { endAt } : {})}\n"
        "      playbackRate={playbackRate}\n"
        "      volume={volume}\n"
        "    />\n"
        "  );\n"
        "};\n";
    return writeFile(compDir + "/AudioClip.tsx", content);
}

bool RemotionExporter::writeTextOverlayTsx(const QString &compDir)
{
    const QString content =
        "import React from 'react';\n"
        "import { useCurrentFrame, useVideoConfig, interpolate, Easing } from 'remotion';\n"
        "\n"
        "interface TextOverlayProps {\n"
        "  text: string;\n"
        "  x?: number;           // 0-1 normalized\n"
        "  y?: number;           // 0-1 normalized\n"
        "  opacity?: number;\n"
        "  rotation?: number;    // degrees\n"
        "  scale?: number;\n"
        "  animIn?: string;      // 'FadeIn' | 'SlideLeft' | 'SlideRight' | 'SlideUp' | 'SlideDown' | 'ScaleIn' | 'Pop'\n"
        "  animOut?: string;\n"
        "  animDuration?: number; // seconds\n"
        "  fontSize?: number;\n"
        "  color?: string;\n"
        "}\n"
        "\n"
        "export const TextOverlay: React.FC<TextOverlayProps> = ({\n"
        "  text,\n"
        "  x = 0.5,\n"
        "  y = 0.85,\n"
        "  opacity = 1,\n"
        "  rotation = 0,\n"
        "  scale = 1,\n"
        "  animIn = 'FadeIn',\n"
        "  animOut = 'None',\n"
        "  animDuration = 0.5,\n"
        "  fontSize = 48,\n"
        "  color = '#ffffff',\n"
        "}) => {\n"
        "  const frame = useCurrentFrame();\n"
        "  const { fps, durationInFrames, width, height } = useVideoConfig();\n"
        "  const animFrames = Math.round(animDuration * fps);\n"
        "\n"
        "  // In animation\n"
        "  const inProgress = interpolate(frame, [0, animFrames], [0, 1], {\n"
        "    extrapolateLeft: 'clamp',\n"
        "    extrapolateRight: 'clamp',\n"
        "    easing: Easing.out(Easing.ease),\n"
        "  });\n"
        "\n"
        "  // Out animation\n"
        "  const outProgress = interpolate(\n"
        "    frame,\n"
        "    [durationInFrames - animFrames, durationInFrames],\n"
        "    [0, 1],\n"
        "    { extrapolateLeft: 'clamp', extrapolateRight: 'clamp', easing: Easing.in(Easing.ease) }\n"
        "  );\n"
        "\n"
        "  const computeTransform = (anim: string, progress: number): React.CSSProperties => {\n"
        "    switch (anim) {\n"
        "      case 'FadeIn':    return { opacity: progress };\n"
        "      case 'FadeOut':   return { opacity: 1 - progress };\n"
        "      case 'SlideLeft': return { transform: `translateX(${interpolate(progress, [0, 1], [120, 0])}px)`, opacity: progress };\n"
        "      case 'SlideRight':return { transform: `translateX(${interpolate(progress, [0, 1], [-120, 0])}px)`, opacity: progress };\n"
        "      case 'SlideUp':   return { transform: `translateY(${interpolate(progress, [0, 1], [60, 0])}px)`, opacity: progress };\n"
        "      case 'SlideDown': return { transform: `translateY(${interpolate(progress, [0, 1], [-60, 0])}px)`, opacity: progress };\n"
        "      case 'ScaleIn':   return { transform: `scale(${progress})`, opacity: progress };\n"
        "      case 'Pop':       return {\n"
        "        transform: `scale(${interpolate(progress, [0, 0.7, 1], [0, 1.2, 1])})`,\n"
        "        opacity: progress,\n"
        "      };\n"
        "      default:          return { opacity: 1 };\n"
        "    }\n"
        "  };\n"
        "\n"
        "  const inStyle  = animIn  !== 'None' ? computeTransform(animIn,  inProgress)  : {};\n"
        "  const outStyle = animOut !== 'None' ? computeTransform(animOut, outProgress) : {};\n"
        "\n"
        "  const finalOpacity = (inStyle.opacity ?? 1) * (1 - (outStyle.opacity !== undefined ? 1 - (outStyle.opacity as number) : 0)) * opacity;\n"
        "  const inTransform  = (inStyle.transform  as string) ?? '';\n"
        "  const outTransform = (outStyle.transform as string) ?? '';\n"
        "  const combinedTransform = [inTransform, outTransform].filter(Boolean).join(' ');\n"
        "\n"
        "  return (\n"
        "    <div\n"
        "      style={{\n"
        "        position: 'absolute',\n"
        "        left: `${x * 100}%`,\n"
        "        top: `${y * 100}%`,\n"
        "        transform: `translate(-50%, -50%) rotate(${rotation}deg) scale(${scale}) ${combinedTransform}`,\n"
        "        opacity: finalOpacity,\n"
        "        fontSize,\n"
        "        color,\n"
        "        fontWeight: 'bold',\n"
        "        textAlign: 'center',\n"
        "        whiteSpace: 'nowrap',\n"
        "        textShadow: '2px 2px 4px rgba(0,0,0,0.8)',\n"
        "        pointerEvents: 'none',\n"
        "        userSelect: 'none',\n"
        "      }}\n"
        "    >\n"
        "      {text}\n"
        "    </div>\n"
        "  );\n"
        "};\n";
    return writeFile(compDir + "/TextOverlay.tsx", content);
}

bool RemotionExporter::writeImageOverlayTsx(const QString &compDir)
{
    const QString content =
        "import React from 'react';\n"
        "import { Img, useCurrentFrame, useVideoConfig, interpolate } from 'remotion';\n"
        "\n"
        "interface ImageOverlayProps {\n"
        "  src: string;\n"
        "  x?: number;        // 0-1 normalized\n"
        "  y?: number;\n"
        "  width?: number;    // 0-1 normalized, 0 = auto\n"
        "  height?: number;\n"
        "  opacity?: number;\n"
        "  rotation?: number;\n"
        "  scale?: number;\n"
        "  fit?: 'contain' | 'cover' | 'fill';\n"
        "}\n"
        "\n"
        "export const ImageOverlay: React.FC<ImageOverlayProps> = ({\n"
        "  src,\n"
        "  x = 0.5,\n"
        "  y = 0.5,\n"
        "  width = 0.3,\n"
        "  height = 0,\n"
        "  opacity = 1,\n"
        "  rotation = 0,\n"
        "  scale = 1,\n"
        "  fit = 'contain',\n"
        "}) => {\n"
        "  const { width: vw, height: vh } = useVideoConfig();\n"
        "  const px = x * vw;\n"
        "  const py = y * vh;\n"
        "  const pw = width > 0 ? width * vw : undefined;\n"
        "  const ph = height > 0 ? height * vh : undefined;\n"
        "\n"
        "  return (\n"
        "    <Img\n"
        "      src={src}\n"
        "      style={{\n"
        "        position: 'absolute',\n"
        "        left: px,\n"
        "        top: py,\n"
        "        width: pw,\n"
        "        height: ph,\n"
        "        objectFit: fit,\n"
        "        opacity,\n"
        "        transform: `translate(-50%, -50%) rotate(${rotation}deg) scale(${scale})`,\n"
        "      }}\n"
        "    />\n"
        "  );\n"
        "};\n";
    return writeFile(compDir + "/ImageOverlay.tsx", content);
}

bool RemotionExporter::writeTransitionTsx(const QString &compDir)
{
    const QString content =
        "import React from 'react';\n"
        "import { AbsoluteFill, interpolate, useCurrentFrame } from 'remotion';\n"
        "\n"
        "export type TransitionType = 'crossfade' | 'wipeLeft' | 'wipeRight' | 'wipeUp' | 'wipeDown' | 'dissolve';\n"
        "\n"
        "interface TransitionProps {\n"
        "  from: React.ReactNode;\n"
        "  to: React.ReactNode;\n"
        "  durationInFrames: number;\n"
        "  type?: TransitionType;\n"
        "}\n"
        "\n"
        "export const Transition: React.FC<TransitionProps> = ({\n"
        "  from,\n"
        "  to,\n"
        "  durationInFrames,\n"
        "  type = 'crossfade',\n"
        "}) => {\n"
        "  const frame = useCurrentFrame();\n"
        "  const progress = interpolate(frame, [0, durationInFrames], [0, 1], {\n"
        "    extrapolateLeft: 'clamp',\n"
        "    extrapolateRight: 'clamp',\n"
        "  });\n"
        "\n"
        "  if (type === 'crossfade' || type === 'dissolve') {\n"
        "    return (\n"
        "      <AbsoluteFill>\n"
        "        <AbsoluteFill style={{ opacity: 1 - progress }}>{from}</AbsoluteFill>\n"
        "        <AbsoluteFill style={{ opacity: progress }}>{to}</AbsoluteFill>\n"
        "      </AbsoluteFill>\n"
        "    );\n"
        "  }\n"
        "\n"
        "  // Wipe transitions\n"
        "  const wipeStyles: Record<TransitionType, React.CSSProperties> = {\n"
        "    crossfade:  {},\n"
        "    dissolve:   {},\n"
        "    wipeLeft:   { clipPath: `inset(0 ${(1 - progress) * 100}% 0 0)` },\n"
        "    wipeRight:  { clipPath: `inset(0 0 0 ${(1 - progress) * 100}%)` },\n"
        "    wipeUp:     { clipPath: `inset(0 0 ${(1 - progress) * 100}% 0)` },\n"
        "    wipeDown:   { clipPath: `inset(${(1 - progress) * 100}% 0 0 0)` },\n"
        "  };\n"
        "\n"
        "  return (\n"
        "    <AbsoluteFill>\n"
        "      <AbsoluteFill>{from}</AbsoluteFill>\n"
        "      <AbsoluteFill style={wipeStyles[type]}>{to}</AbsoluteFill>\n"
        "    </AbsoluteFill>\n"
        "  );\n"
        "};\n";
    return writeFile(compDir + "/Transition.tsx", content);
}

bool RemotionExporter::writeEffectsTsx(const QString &compDir)
{
    const QString content =
        "import React from 'react';\n"
        "import { AbsoluteFill, useCurrentFrame, useVideoConfig, interpolate } from 'remotion';\n"
        "\n"
        "// --- Vignette Effect ---\n"
        "interface VignetteProps {\n"
        "  intensity?: number;  // 0-1\n"
        "  radius?: number;     // 0-1\n"
        "}\n"
        "\n"
        "export const Vignette: React.FC<VignetteProps> = ({ intensity = 0.5, radius = 0.8 }) => {\n"
        "  const r = radius * 100;\n"
        "  const i = intensity;\n"
        "  return (\n"
        "    <AbsoluteFill\n"
        "      style={{\n"
        "        background: `radial-gradient(ellipse at center, transparent ${r}%, rgba(0,0,0,${i}) 100%)`,\n"
        "        pointerEvents: 'none',\n"
        "      }}\n"
        "    />\n"
        "  );\n"
        "};\n"
        "\n"
        "// --- ChromaKey Hint (note: true chroma key requires video processing) ---\n"
        "interface ChromaKeyHintProps {\n"
        "  color?: string;\n"
        "  tolerance?: number;\n"
        "}\n"
        "\n"
        "export const ChromaKeyHint: React.FC<ChromaKeyHintProps> = ({ color = '#00ff00', tolerance = 0.15 }) => {\n"
        "  // True chroma key is not achievable with CSS alone.\n"
        "  // Use remotion-chromakey or pre-process the video.\n"
        "  return null;\n"
        "};\n"
        "\n"
        "// --- Color Correction Overlay via CSS filter ---\n"
        "interface ColorCorrectionProps {\n"
        "  brightness?: number;  // CSS 0-2, 1=normal\n"
        "  contrast?: number;\n"
        "  saturation?: number;\n"
        "  hue?: number;         // degrees\n"
        "  children: React.ReactNode;\n"
        "}\n"
        "\n"
        "export const ColorCorrection: React.FC<ColorCorrectionProps> = ({\n"
        "  brightness = 1,\n"
        "  contrast = 1,\n"
        "  saturation = 1,\n"
        "  hue = 0,\n"
        "  children,\n"
        "}) => {\n"
        "  const filter = [\n"
        "    brightness !== 1 ? `brightness(${brightness})` : '',\n"
        "    contrast   !== 1 ? `contrast(${contrast})`     : '',\n"
        "    saturation !== 1 ? `saturate(${saturation})`   : '',\n"
        "    hue        !== 0 ? `hue-rotate(${hue}deg)`     : '',\n"
        "  ].filter(Boolean).join(' ');\n"
        "\n"
        "  return (\n"
        "    <AbsoluteFill style={{ filter: filter || undefined }}>\n"
        "      {children}\n"
        "    </AbsoluteFill>\n"
        "  );\n"
        "};\n"
        "\n"
        "// --- Mosaic / Pixelate (SVG filter) ---\n"
        "interface MosaicProps {\n"
        "  blockSize?: number;  // pixels\n"
        "  children: React.ReactNode;\n"
        "}\n"
        "\n"
        "export const Mosaic: React.FC<MosaicProps> = ({ blockSize = 10, children }) => {\n"
        "  const filterId = 'mosaic-filter';\n"
        "  return (\n"
        "    <AbsoluteFill>\n"
        "      <svg width=\"0\" height=\"0\" style={{ position: 'absolute' }}>\n"
        "        <defs>\n"
        "          <filter id={filterId}>\n"
        "            <feImage href=\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg'/>\" />\n"
        "            <feTurbulence type=\"fractalNoise\" baseFrequency={1 / blockSize} />\n"
        "            <feDisplacementMap in=\"SourceGraphic\" scale={blockSize} />\n"
        "          </filter>\n"
        "        </defs>\n"
        "      </svg>\n"
        "      <AbsoluteFill style={{ filter: `url(#${filterId})` }}>\n"
        "        {children}\n"
        "      </AbsoluteFill>\n"
        "    </AbsoluteFill>\n"
        "  );\n"
        "};\n";
    return writeFile(compDir + "/Effects.tsx", content);
}

// ---------------------------------------------------------------------------
// Asset copy
// ---------------------------------------------------------------------------

bool RemotionExporter::copyAssets(const QString &assetsDir, const ProjectData &data)
{
    QDir().mkpath(assetsDir);

    auto copyClips = [&](const QVector<QVector<ClipInfo>> &tracks) -> bool {
        for (const auto &track : tracks) {
            for (const auto &clip : track) {
                if (clip.filePath.isEmpty()) continue;
                QFileInfo fi(clip.filePath);
                if (!fi.exists()) continue;
                const QString dest = assetsDir + "/" + fi.fileName();
                if (!QFile::exists(dest)) {
                    if (!QFile::copy(clip.filePath, dest)) {
                        emit exportError(QString("Failed to copy asset: %1").arg(clip.filePath));
                        return false;
                    }
                }
            }
        }
        return true;
    };

    if (!copyClips(data.videoTracks)) return false;
    if (!copyClips(data.audioTracks)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// RemotionPreview
// ---------------------------------------------------------------------------

RemotionPreview::RemotionPreview(QObject *parent)
    : QObject(parent)
    , m_process(new QProcess(this))
{
    connect(m_process, &QProcess::readyReadStandardOutput, this, &RemotionPreview::onReadyRead);
    connect(m_process, &QProcess::readyReadStandardError,  this, &RemotionPreview::onReadyRead);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RemotionPreview::onFinished);
}

RemotionPreview::~RemotionPreview()
{
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        m_process->waitForFinished(3000);
    }
}

void RemotionPreview::launchPreview(const QString &projectDir)
{
    if (m_process->state() != QProcess::NotRunning)
        m_process->terminate();

    m_process->setWorkingDirectory(projectDir);
    m_process->start("npx", QStringList() << "remotion" << "preview");
}

void RemotionPreview::launchRender(const QString &projectDir, const QString &outputPath)
{
    if (m_process->state() != QProcess::NotRunning)
        m_process->terminate();

    m_process->setWorkingDirectory(projectDir);
    m_process->start("npx", QStringList() << "remotion" << "render"
                     << "src/index.ts" << "VideoComposition" << outputPath);
}

void RemotionPreview::terminate()
{
    m_process->terminate();
}

void RemotionPreview::onReadyRead()
{
    const QString out = m_process->readAllStandardOutput();
    const QString err = m_process->readAllStandardError();
    if (!out.isEmpty()) emit processOutput(out);
    if (!err.isEmpty()) emit processOutput(err);
}

void RemotionPreview::onFinished(int exitCode, QProcess::ExitStatus /*status*/)
{
    emit processFinished(exitCode);
}

// ---------------------------------------------------------------------------
// RemotionExportDialog
// ---------------------------------------------------------------------------

RemotionExportDialog::RemotionExportDialog(const ProjectConfig &project,
                                            const ProjectData  &data,
                                            QWidget *parent)
    : QDialog(parent)
    , m_data(data)
    , m_exporter(new RemotionExporter(this))
{
    setWindowTitle("Export to Remotion");
    setMinimumWidth(560);

    m_config.fps    = project.fps;
    m_config.width  = project.width;
    m_config.height = project.height;
    m_config.projectName = project.name.isEmpty() ? "my-remotion-video" : project.name;

    connect(m_exporter, &RemotionExporter::exportProgress, this, &RemotionExportDialog::onExportProgress);
    connect(m_exporter, &RemotionExporter::exportComplete, this, &RemotionExportDialog::onExportComplete);
    connect(m_exporter, &RemotionExporter::exportError,    this, &RemotionExportDialog::onExportError);

    setupUI();
    updateStructurePreview();
}

void RemotionExportDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);

    // --- Project settings group ---
    auto *projectGroup  = new QGroupBox("Project Settings");
    auto *projectForm   = new QFormLayout(projectGroup);

    m_projectNameEdit = new QLineEdit(m_config.projectName, this);
    projectForm->addRow("Project Name:", m_projectNameEdit);

    auto *dirLayout   = new QHBoxLayout;
    m_outputDirEdit   = new QLineEdit(this);
    m_outputDirEdit->setPlaceholderText("Select output directory...");
    auto *browseBtn   = new QPushButton("Browse...", this);
    dirLayout->addWidget(m_outputDirEdit);
    dirLayout->addWidget(browseBtn);
    projectForm->addRow("Output Directory:", dirLayout);

    mainLayout->addWidget(projectGroup);

    // --- Composition settings group ---
    auto *compGroup = new QGroupBox("Composition");
    auto *compForm  = new QFormLayout(compGroup);

    m_fpsSpin = new QSpinBox(this);
    m_fpsSpin->setRange(1, 120);
    m_fpsSpin->setValue(m_config.fps);
    compForm->addRow("FPS:", m_fpsSpin);

    auto *resLayout = new QHBoxLayout;
    m_widthSpin  = new QSpinBox(this);
    m_heightSpin = new QSpinBox(this);
    m_widthSpin->setRange(1, 7680);
    m_heightSpin->setRange(1, 4320);
    m_widthSpin->setValue(m_config.width);
    m_heightSpin->setValue(m_config.height);
    resLayout->addWidget(m_widthSpin);
    resLayout->addWidget(new QLabel("x", this));
    resLayout->addWidget(m_heightSpin);
    resLayout->addStretch();
    compForm->addRow("Resolution:", resLayout);

    mainLayout->addWidget(compGroup);

    // --- Options group ---
    auto *optGroup  = new QGroupBox("Options");
    auto *optLayout = new QVBoxLayout(optGroup);

    m_includeAssetsCheck   = new QCheckBox("Copy media files to assets/ folder", this);
    m_includeAssetsCheck->setChecked(true);
    m_generatePkgJsonCheck = new QCheckBox("Generate package.json", this);
    m_generatePkgJsonCheck->setChecked(true);
    m_runNpmCheck          = new QCheckBox("Run npm install after export", this);
    m_runNpmCheck->setChecked(false);

    optLayout->addWidget(m_includeAssetsCheck);
    optLayout->addWidget(m_generatePkgJsonCheck);
    optLayout->addWidget(m_runNpmCheck);
    mainLayout->addWidget(optGroup);

    // --- Structure preview ---
    auto *previewGroup  = new QGroupBox("Generated Structure Preview");
    auto *previewLayout = new QVBoxLayout(previewGroup);
    m_structureTree = new QTreeWidget(this);
    m_structureTree->setHeaderHidden(true);
    m_structureTree->setMaximumHeight(200);
    previewLayout->addWidget(m_structureTree);
    mainLayout->addWidget(previewGroup);

    // --- Progress ---
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(false);
    mainLayout->addWidget(m_progressBar);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #555; font-size: 12px; padding: 2px;");
    mainLayout->addWidget(m_statusLabel);

    // --- Buttons ---
    auto *btnLayout = new QHBoxLayout;
    m_exportBtn     = new QPushButton("Export", this);
    m_exportOpenBtn = new QPushButton("Export && Open in VS Code", this);
    auto *cancelBtn = new QPushButton("Cancel", this);
    btnLayout->addWidget(m_exportBtn);
    btnLayout->addWidget(m_exportOpenBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(cancelBtn);
    mainLayout->addLayout(btnLayout);

    // Connections
    connect(browseBtn,      &QPushButton::clicked, this, &RemotionExportDialog::onBrowseOutput);
    connect(m_exportBtn,    &QPushButton::clicked, this, &RemotionExportDialog::onExport);
    connect(m_exportOpenBtn,&QPushButton::clicked, this, &RemotionExportDialog::onExportAndOpen);
    connect(cancelBtn,      &QPushButton::clicked, this, &QDialog::reject);

    connect(m_projectNameEdit, &QLineEdit::textChanged, this, &RemotionExportDialog::updateStructurePreview);
    connect(m_includeAssetsCheck, &QCheckBox::toggled, this, &RemotionExportDialog::updateStructurePreview);
    connect(m_generatePkgJsonCheck, &QCheckBox::toggled, this, &RemotionExportDialog::updateStructurePreview);
}

void RemotionExportDialog::onBrowseOutput()
{
    const QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory");
    if (!dir.isEmpty()) {
        m_outputDirEdit->setText(dir);
        updateStructurePreview();
    }
}

void RemotionExportDialog::updateStructurePreview()
{
    m_structureTree->clear();

    const QString name = m_projectNameEdit->text().isEmpty()
                       ? "my-remotion-video"
                       : m_projectNameEdit->text();
    const bool pkg     = m_generatePkgJsonCheck->isChecked();
    const bool assets  = m_includeAssetsCheck->isChecked();

    auto *root = new QTreeWidgetItem(m_structureTree, QStringList() << name + "/");

    if (pkg) {
        new QTreeWidgetItem(root, QStringList() << "package.json");
        new QTreeWidgetItem(root, QStringList() << "tsconfig.json");
    }

    auto *src = new QTreeWidgetItem(root, QStringList() << "src/");
    new QTreeWidgetItem(src, QStringList() << "Root.tsx");
    new QTreeWidgetItem(src, QStringList() << "Video.tsx");

    auto *comp = new QTreeWidgetItem(src, QStringList() << "components/");
    new QTreeWidgetItem(comp, QStringList() << "VideoClip.tsx");
    new QTreeWidgetItem(comp, QStringList() << "AudioClip.tsx");
    new QTreeWidgetItem(comp, QStringList() << "TextOverlay.tsx");
    new QTreeWidgetItem(comp, QStringList() << "ImageOverlay.tsx");
    new QTreeWidgetItem(comp, QStringList() << "Transition.tsx");
    new QTreeWidgetItem(comp, QStringList() << "Effects.tsx");

    auto *lib = new QTreeWidgetItem(src, QStringList() << "lib/");
    new QTreeWidgetItem(lib, QStringList() << "timeline.ts");

    if (assets) {
        auto *assetNode = new QTreeWidgetItem(src, QStringList() << "assets/");
        int count = 0;
        auto countClips = [&](const QVector<QVector<ClipInfo>> &tracks) {
            for (const auto &track : tracks) {
                for (const auto &clip : track) {
                    if (!clip.filePath.isEmpty()) {
                        QFileInfo fi(clip.filePath);
                        new QTreeWidgetItem(assetNode, QStringList() << fi.fileName());
                        ++count;
                        if (count >= 5) return;
                    }
                }
            }
        };
        countClips(m_data.videoTracks);
        if (count < 5) countClips(m_data.audioTracks);
        if (count == 0)
            new QTreeWidgetItem(assetNode, QStringList() << "(media files copied here)");
    }

    m_structureTree->expandAll();
}

void RemotionExportDialog::setControlsEnabled(bool enabled)
{
    m_projectNameEdit->setEnabled(enabled);
    m_outputDirEdit->setEnabled(enabled);
    m_fpsSpin->setEnabled(enabled);
    m_widthSpin->setEnabled(enabled);
    m_heightSpin->setEnabled(enabled);
    m_includeAssetsCheck->setEnabled(enabled);
    m_generatePkgJsonCheck->setEnabled(enabled);
    m_runNpmCheck->setEnabled(enabled);
    m_exportBtn->setEnabled(enabled);
    m_exportOpenBtn->setEnabled(enabled);
}

void RemotionExportDialog::onExport()
{
    m_openAfterExport = false;

    if (m_outputDirEdit->text().isEmpty()) {
        onBrowseOutput();
        if (m_outputDirEdit->text().isEmpty()) return;
    }

    m_config.outputDir          = m_outputDirEdit->text();
    m_config.projectName        = m_projectNameEdit->text().isEmpty()
                                ? "my-remotion-video"
                                : m_projectNameEdit->text();
    m_config.fps                = m_fpsSpin->value();
    m_config.width              = m_widthSpin->value();
    m_config.height             = m_heightSpin->value();
    m_config.includeAssets      = m_includeAssetsCheck->isChecked();
    m_config.generatePackageJson = m_generatePkgJsonCheck->isChecked();

    setControlsEnabled(false);
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_statusLabel->setText("Exporting...");

    m_exporter->exportProject(m_config, m_data);
}

void RemotionExportDialog::onExportAndOpen()
{
    m_openAfterExport = true;
    onExport();
}

void RemotionExportDialog::onExportProgress(int percent)
{
    m_progressBar->setValue(percent);
}

void RemotionExportDialog::onExportComplete(QString outputDir)
{
    m_progressBar->setValue(100);
    m_statusLabel->setText(QString("Export complete: %1").arg(outputDir));

    if (m_runNpmCheck->isChecked())
        runNpmInstall(outputDir);

    if (m_openAfterExport) {
        // Try to open VS Code
        QProcess::startDetached("code", QStringList() << outputDir);
    }

    QMessageBox::information(this, "Export Complete",
        QString("Remotion project exported to:\n%1\n\n"
                "Run 'npm install && npx remotion preview' in that directory to preview.").arg(outputDir));

    setControlsEnabled(true);
    accept();
}

void RemotionExportDialog::onExportError(QString message)
{
    m_progressBar->setVisible(false);
    m_statusLabel->setText(QString("Error: %1").arg(message));
    setControlsEnabled(true);
    QMessageBox::critical(this, "Export Error", message);
}

void RemotionExportDialog::runNpmInstall(const QString &projectDir)
{
    m_statusLabel->setText("Running npm install...");
    QProcess npm;
    npm.setWorkingDirectory(projectDir);
    npm.start("npm", QStringList() << "install");
    npm.waitForFinished(60000);
    if (npm.exitCode() == 0)
        m_statusLabel->setText("npm install complete.");
    else
        m_statusLabel->setText("npm install failed (check that Node.js is installed).");
}
