cmake_minimum_required(VERSION 3.20)
project(v-simple-editor VERSION 0.14.0 LANGUAGES CXX)

set(VEDITOR_EDITION "classic" CACHE STRING "Build edition: classic (H.264/HEVC) | modern (+ AV1)")
set_property(CACHE VEDITOR_EDITION PROPERTY STRINGS classic modern)
message(STATUS "VEDITOR_EDITION = ${VEDITOR_EDITION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Multimedia MultimediaWidgets OpenGL OpenGLWidgets Network)

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
    src/ProjectSettings.cpp
    src/ExportDialog.cpp
    src/Exporter.cpp
    src/CodecDetector.cpp
    src/UndoManager.cpp
    src/Overlay.cpp
    src/OverlayDialogs.cpp
    src/VideoEffect.cpp
    src/VideoEffectDialogs.cpp
    src/ColorWheelWidget.cpp
    src/ColorGradingPanel.cpp
    src/Keyframe.cpp
    src/EffectPlugin.cpp
    src/ProjectFile.cpp
    src/GLPreview.cpp
    src/ShaderEffect.cpp
    src/WaveformGenerator.cpp
    src/AutoEdit.cpp
    src/ThemeManager.cpp
    src/MultiCam.cpp
    src/TextManager.cpp
    src/TextInteractive.cpp
    src/GradientStopBar.cpp
    src/EffectPreset.cpp
    src/SubtitleGenerator.cpp
    src/MotionTracker.cpp
    src/TrackerLink.cpp
    src/NoiseReduction.cpp
    src/ResourceGuide.cpp
    src/AutoSave.cpp
    src/ProxyManager.cpp
    src/ProxyProgressDialog.cpp
    src/ProxyManagementDialog.cpp
    src/DecoderSlotManager.cpp
    src/LutImporter.cpp
    src/TimelineMarker.cpp
    src/VideoStabilizer.cpp
    src/SpeedRamp.cpp
    src/RenderQueue.cpp
    src/AudioEQ.cpp
    src/ScreenRecorder.cpp
    src/AIHighlight.cpp
    src/LayerCompositor.cpp
    src/TransformAnimator.cpp
    src/MaskSystem.cpp
    src/ParticleSystem.cpp
    src/TextAnimator.cpp
    src/ShapeLayer.cpp
    src/Camera3D.cpp
    src/Expression.cpp
    src/Precompose.cpp
    src/Rotoscope.cpp
    src/WarpDistortion.cpp
    src/RecentFiles.cpp
    src/ShortcutEditor.cpp
    src/VSTHost.cpp
    src/NetworkRender.cpp
    src/RemotionExport.cpp
    src/PythonScript.cpp
    src/SplashScreen.h
    src/WelcomeWidget.h
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
    ${AVFORMAT_LIBRARY}
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWSCALE_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${AVFILTER_LIBRARY}
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE dbghelp)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    VEDITOR_EDITION="${VEDITOR_EDITION}"
    $<$<STREQUAL:${VEDITOR_EDITION},modern>:VEDITOR_AV1=1>
)
