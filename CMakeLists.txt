cmake_minimum_required(VERSION 3.20)
project(v-editor-simple VERSION 0.14.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Multimedia MultimediaWidgets OpenGL OpenGLWidgets Network)

# FFmpeg via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(AVFILTER REQUIRED libavfilter)

add_executable(${PROJECT_NAME}
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
    src/EffectPreset.cpp
    src/SubtitleGenerator.cpp
    src/MotionTracker.cpp
    src/TrackerLink.cpp
    src/NoiseReduction.cpp
    src/ResourceGuide.cpp
    src/AutoSave.cpp
    src/ProxyManager.cpp
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
    resources/resources.qrc
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVFILTER_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Network
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVFILTER_LIBRARIES}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
)
