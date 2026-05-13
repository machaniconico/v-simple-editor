cmake_minimum_required(VERSION 3.20)
project(v-simple-editor VERSION 0.14.0 LANGUAGES CXX)

set(VEDITOR_EDITION "classic" CACHE STRING "Build edition: classic (H.264/HEVC) | modern (+ AV1)")
set_property(CACHE VEDITOR_EDITION PROPERTY STRINGS classic modern)
message(STATUS "VEDITOR_EDITION = ${VEDITOR_EDITION}")
option(VEDITOR_PLANAR_TRACKER_SELFTEST "Enable the planar tracker synthetic self-test helper" OFF)
option(VEDITOR_EFFECT_PARAM_SCHEMA_SELFTEST "Enable the effect param schema round-trip self-test" OFF)
option(VEDITOR_BRUSH_SELFTEST "Enable the brush animation synthetic self-test helper" OFF)
option(VEDITOR_SNSPACK_SELFTEST "Enable the SNS pack synthetic self-test" OFF)
option(VEDITOR_NODEGRAPH_SELFTEST "Enable the node graph synthetic self-test" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Multimedia MultimediaWidgets OpenGL OpenGLWidgets Network Concurrent)

# FFmpeg — find headers and libraries (works with vcpkg on all platforms)
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h)
find_library(AVFORMAT_LIBRARY avformat)
find_library(AVCODEC_LIBRARY avcodec)
find_library(AVUTIL_LIBRARY avutil)
find_library(SWSCALE_LIBRARY swscale)
find_library(SWRESAMPLE_LIBRARY swresample)
find_library(AVFILTER_LIBRARY avfilter)

if(NOT AVFORMAT_LIBRARY)
    message(FATAL_ERROR "FFmpeg not found. Install via: vcpkg install ffmpeg")
endif()

set(APP_EXECUTABLE_TYPE "")
set(APP_RESOURCES "")
if(WIN32)
    set(APP_EXECUTABLE_TYPE WIN32)
    set(APP_RESOURCES resources/app-icon.rc)
endif()

add_executable(${PROJECT_NAME} ${APP_EXECUTABLE_TYPE}
    src/main.cpp
    src/MainWindow.cpp
    src/VideoPlayer.cpp
    src/Timeline.cpp
    src/SnapEngine.cpp
    src/ProjectSettings.cpp
    src/ExportDialog.cpp
    src/Exporter.cpp
    src/CodecDetector.cpp
    src/UndoManager.cpp
    src/Overlay.cpp
    src/OverlayDialogs.cpp
    src/VideoEffect.cpp
    src/FractalNoise.h
    src/FractalNoise.cpp
    src/EffectParamSchema.h
    src/EffectParamSchema.cpp
    src/VideoEffectDialogs.cpp
    src/ColorWheelWidget.cpp
    src/ColorGradingPanel.cpp
    src/EffectControlsPanel.h
    src/EffectControlsPanel.cpp
    src/MotionSectionWidget.h
    src/MotionSectionWidget.cpp
    src/EffectRowWidget.h
    src/EffectRowWidget.cpp
    src/EffectKeyframeNavBar.h
    src/EffectKeyframeNavBar.cpp
    src/EffectKeyframeToggle.h
    src/EffectKeyframeToggle.cpp
    src/EffectControlsToolbar.h
    src/EffectControlsToolbar.cpp
    src/EffectClipboard.h
    src/EffectClipboard.cpp
    src/PasteAttributesDialog.h
    src/PasteAttributesDialog.cpp
    src/CurveEditor.cpp
    src/HueVsSatEditor.cpp
    src/HueVsSatEditor.h
    src/Keyframe.cpp
    src/EffectPlugin.cpp
    src/PluginManifest.cpp
    src/HDRTransfer.cpp
    src/AIUpscale.cpp
    src/FrameInterpolator.cpp
    src/ProjectFile.cpp
    src/ProjectCollector.cpp
    src/ProjectCollector.h
    src/ProjectCollectorDialog.h
    src/ProjectCollectorDialog.cpp
    src/SceneCutScanner.h
    src/SceneCutScanner.cpp
    src/SceneCutDialog.h
    src/SceneCutDialog.cpp
    src/AudioClipEditor.h
    src/AudioClipEditor.cpp
    src/AudioDucking.h
    src/AudioDucking.cpp
    src/AudioDuckingDialog.h
    src/AudioDuckingDialog.cpp
    src/GLPreview.cpp
    src/D3D11Bridge.cpp
    src/ShaderEffect.cpp
    src/WaveformGenerator.cpp
    src/AutoEdit.cpp
    src/ThemeManager.cpp
    src/MultiCam.cpp
    src/MultiCamDialog.cpp
    src/MultiCamDialog.h
    src/TextManager.cpp
    src/TitlePresetDialog.cpp
    src/TextInteractive.cpp
    src/GradientStopBar.cpp
    src/EffectPreset.cpp
    src/SubtitleGenerator.cpp
    src/SubtitleTrackRenderer.h
    src/SubtitleTrackRenderer.cpp
    src/MagneticTimeline.h
    src/MagneticTimeline.cpp
    src/CaptionTrack.h
    src/CaptionTrack.cpp
    src/CaptionStyle.h
    src/CaptionStyle.cpp
    src/SubtitleIO.h
    src/SubtitleIO.cpp
    src/CaptionEditorDialog.h
    src/CaptionEditorDialog.cpp
    src/SpeechRecognizer.h
    src/SpeechRecognizer.cpp
    src/MotionTracker.cpp
    src/MotionStabilizer.cpp
    src/TrackerLink.cpp
    src/NoiseReduction.cpp
    src/ResourceGuide.cpp
    src/AutoSave.cpp
    src/ProxyManager.cpp
    src/ProxyProgressDialog.cpp
    src/ProxyManagementDialog.cpp
    src/DecoderSlotManager.cpp
    src/AudioMixer.cpp
    src/PlanarTracker.cpp
    src/PlanarTracker.h
    src/SurfaceTool.cpp
    src/SurfaceTool.h
    src/EqualizerPanel.cpp
    src/EqualizerPanel.h
    src/CompressorPanel.cpp
    src/CompressorPanel.h
    src/ReverbPanel.cpp
    src/ReverbPanel.h
    src/LoudnessPanel.cpp
    src/LoudnessPanel.h
    src/LoudnessAnalyzer.h
    src/LoudnessAnalyzer.cpp
    src/NoiseReductionPanel.cpp
    src/NoiseReductionPanel.h
    src/AudioMeterWidget.cpp
    src/AudioMeterWidget.h
    src/HistoryDockWidget.cpp
    src/HistoryDockWidget.h
    src/LutImporter.cpp
    src/TimelineMarker.cpp
    src/AdjustmentLayer.cpp
    src/AdjustmentLayer.h
    src/VideoStabilizer.cpp
    src/SpeedRamp.cpp
    src/SpeedRampData.cpp
    src/SpeedRampData.h
    src/RenderQueue.cpp
    src/RenderQueueDialog.cpp
    src/RenderQueueDialog.h
    src/AudioEQ.cpp
    src/AspectReframer.h
    src/AspectReframer.cpp
    src/ScreenRecorder.cpp
    src/VoiceOverRecorder.h
    src/VoiceOverRecorder.cpp
    src/VoiceOverDialog.h
    src/VoiceOverDialog.cpp
    src/AIHighlight.cpp
    src/AIMask.h
    src/AIMask.cpp
    src/AIMaskDialog.h
    src/AIMaskDialog.cpp
    src/SceneDetector.cpp
    src/LayerCompositor.cpp
    src/TransformAnimator.cpp
    src/MaskSystem.cpp
    src/ParticleSystem.cpp
    src/ParticleEffectDialog.h
    src/ParticleEffectDialog.cpp
    src/TextAnimator.cpp
    src/VfxControlsPanel.h
    src/VfxControlsPanel.cpp
    src/VariableFontAxis.h
    src/VariableFontAxis.cpp
    src/TextAnimPresets.h
    src/TextAnimPresets.cpp
    src/MographText.h
    src/MographText.cpp
    src/TextMaskReveal.h
    src/TextMaskReveal.cpp
    src/PathText.h
    src/PathText.cpp
    src/Text3DLayer.h
    src/Text3DLayer.cpp
    src/ExtrudedMesh.h
    src/ExtrudedMesh.cpp
    src/SoftRaster3D.h
    src/SoftRaster3D.cpp
    src/ClipExpressionBindings.h
    src/ClipExpressionBindings.cpp
    src/WiggleTransform.h
    src/WiggleTransform.cpp
    src/Text3DExtrusionDialog.h
    src/Text3DExtrusionDialog.cpp
    src/ExpressionBindingDialog.h
    src/ExpressionBindingDialog.cpp
    src/CameraMotionDialog.h
    src/CameraMotionDialog.cpp
    src/TextPathWarp.h
    src/TextPathWarp.cpp
    src/BrushAnimation.h
    src/BrushAnimation.cpp
    src/BrushAnimationDialog.h
    src/BrushAnimationDialog.cpp
    src/SmartReframe.h
    src/SmartReframe.cpp
    src/SmartReframeDialog.h
    src/SmartReframeDialog.cpp
    src/ShapeLayer.cpp
    src/Camera3D.cpp
    src/Expression.cpp
    src/Precompose.cpp
    src/Rotoscope.cpp
    src/RotoAutoTrace.h
    src/RotoAutoTrace.cpp
    src/RotoTracking.h
    src/RotoTracking.cpp
    src/OpticalFlow.h
    src/OpticalFlow.cpp
    src/TimeRemap.h
    src/TimeRemap.cpp
    src/RotoBrushTool.h
    src/RotoBrushTool.cpp
    src/RotoToolsDialog.h
    src/RotoToolsDialog.cpp
    src/TimeRemapDialog.h
    src/TimeRemapDialog.cpp
    src/FavoritesEditDialog.h
    src/FavoritesEditDialog.cpp
    src/WarpDistortion.cpp
    src/NodeGraph.h
    src/NodeGraph.cpp
    src/NodeEvaluator.h
    src/NodeEvaluator.cpp
    src/NodeLibrary.h
    src/NodeLibrary.cpp
    src/NodePropertiesPanel.h
    src/NodePropertiesPanel.cpp
    src/NodeCanvasWidget.h
    src/NodeCanvasWidget.cpp
    src/LayerNodeBridge.h
    src/LayerNodeBridge.cpp
    src/RecentFiles.cpp
    src/ShortcutCustomizeDialog.cpp
    src/ShortcutCustomizeDialog.h

    src/ShortcutManager.cpp
    src/ShortcutManager.h
    src/SocialPreset.h
    src/SocialPreset.cpp
    src/SocialExportDialog.h
    src/SocialExportDialog.cpp
    src/VSTHost.cpp
    src/NetworkRender.cpp
    src/RemotionExport.cpp
    src/PythonScript.cpp
    src/LumetriScopes.cpp
    src/SplashScreen.h
    src/WelcomeWidget.h
    src/HDRSettingsDialog.h
    src/HDRSettingsDialog.cpp
    src/HDRTransfer.h
    src/AIUpscale.h
    src/FrameInterpolator.h
    src/PluginManifest.h
    src/AIProcessingDialog.h
    src/AIProcessingDialog.cpp
    src/PluginBrowserDialog.h
    src/PluginBrowserDialog.cpp
    resources/resources.qrc
    ${APP_RESOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Network
    Qt6::Concurrent
    ${AVFORMAT_LIBRARY}
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWSCALE_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${AVFILTER_LIBRARY}
)

if(WIN32)
    # opengl32 — needed for wglGetCurrentDC and the WGL_NV_DX_interop2 entry
    # points (Phase 1e). Qt6::OpenGL is the Qt wrapper, not the system GL.
    target_link_libraries(${PROJECT_NAME} PRIVATE dbghelp opengl32)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    VEDITOR_EDITION="${VEDITOR_EDITION}"
    $<$<BOOL:${VEDITOR_PLANAR_TRACKER_SELFTEST}>:VEDITOR_PLANAR_TRACKER_SELFTEST=1>
    $<$<BOOL:${VEDITOR_EFFECT_PARAM_SCHEMA_SELFTEST}>:VEDITOR_EFFECT_PARAM_SCHEMA_SELFTEST=1>
    $<$<BOOL:${VEDITOR_BRUSH_SELFTEST}>:VEDITOR_BRUSH_SELFTEST=1>
    $<$<BOOL:${VEDITOR_SNSPACK_SELFTEST}>:VEDITOR_SNSPACK_SELFTEST=1>
    $<$<BOOL:${VEDITOR_NODEGRAPH_SELFTEST}>:VEDITOR_NODEGRAPH_SELFTEST=1>
    $<$<STREQUAL:${VEDITOR_EDITION},modern>:VEDITOR_AV1=1>
)
