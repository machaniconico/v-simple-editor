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

# Sprint 16 / US-AFF-1: SVG rasterization for affinity::vector::loadSvg().
# Optional: when not present, AffinityVectorImporter.cpp returns a null QImage
# and the rest of the build still succeeds (the .cpp uses __has_include guards).
find_package(Qt6 QUIET COMPONENTS Svg)
find_package(Qt6 QUIET COMPONENTS Pdf)

# Sprint 16 / US-BLE-1: optional FBX/Alembic mesh import via libassimp.
# When unavailable, BlenderMeshImporter.cpp falls back to OBJ/glTF only.
find_package(assimp QUIET)

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
    # PRD-B ④ — libavcore shared helpers (Probe + Encode + Concat + Decode + VideoFilterGraph).
    src/libavcore/Probe.h
    src/libavcore/Probe.cpp
    src/libavcore/Encode.h
    src/libavcore/Encode.cpp
    src/libavcore/Concat.h
    src/libavcore/Concat.cpp
    src/libavcore/Decode.h
    src/libavcore/Decode.cpp
    src/libavcore/VideoFilterGraph.h
    src/libavcore/VideoFilterGraph.cpp
    src/libavcore/AudioExtract.h
    src/libavcore/AudioExtract.cpp

    # PRD-SPLIT-MAIN-1: selftest registry (table + dispatch helpers).
    src/selftests/SelftestRegistry.h
    src/selftests/SelftestRegistry.cpp

    # PRD-PHASE5B-OAUTH-MOCK-HARNESS: localhost OAuth/upload mock server for e2e selftests.
    src/selftests/oauth_mock_server.h
    src/selftests/oauth_mock_server.cpp
    src/selftests/oauth_refresh_e2e_selftest.cpp
    src/selftests/cred_audit_log_selftest.cpp

    # PRD-SPLIT-MAIN-2 Phase 1: preset selftests (moved from src/main.cpp).
    src/selftests/preset_selftests.cpp

    # PRD-SPLIT-MAIN-2 Phase 2: parity + track-matte selftests (moved from src/main.cpp).
    src/selftests/parity_matte_selftests.cpp
    src/selftests/keyframe_anim_parity_selftest.cpp
    src/selftests/KeyframeLoopSelftest.cpp
    src/selftests/MotionPresetSelftest.cpp
    src/selftests/motion_blur_parity_selftest.cpp
    src/selftests/MotionBlurP2Selftest.cpp
    src/selftests/effect_keyframe_parity_selftest.cpp
    src/selftests/effect_timing_selftest.cpp
    src/selftests/bezier_easing_selftest.cpp
    src/selftests/EasingPresetsSelftest.cpp
    src/selftests/SpatialPathSelftest.cpp
    src/selftests/SmartRenderSelftest.cpp

    # PRD-SPLIT-MAIN-3 Phase 3-A: libavcore-backed selftests (moved from src/main.cpp).
    src/selftests/libavcore_selftests.cpp

    # PRD-SPLIT-MAIN-3 Phase 3-B: effects selftests (moved from src/main.cpp).
    src/selftests/effects_selftests.cpp
    src/selftests/Text3dPreviewSelftest.cpp
    src/selftests/AeFxBlurSelftest.cpp
    src/selftests/AeFxColorSelftest.cpp
    src/selftests/AeFxColor2Selftest.cpp
    src/selftests/AeFxDistortSelftest.cpp
    src/selftests/AeFxDistort2Selftest.cpp
    src/selftests/AeFxGenerateSelftest.cpp
    src/selftests/AeFxStylizeSelftest.cpp

    # PRD-SPLIT-MAIN-3 Phase 3-C: Sprint 11-22 サービス selftests (moved from src/main.cpp).
    src/selftests/sprint_selftests.cpp

    # PRD-SPLIT-MAIN-3 Phase 3-D: misc selftests (moved from src/main.cpp, final phase).
    src/selftests/misc_selftests.cpp

    src/main.cpp
    src/util/CrashHandler.cpp
    src/util/Logger.cpp
    src/util/RcPause.h
    src/MainWindow.cpp
    src/VideoPlayer.cpp
    src/Timeline.cpp
    src/SnapEngine.cpp
    src/ProjectSettings.cpp
    src/SequenceSettingsDialog.h
    src/SequenceSettingsDialog.cpp
    src/ExportDialog.cpp
    src/ExportRange.h
    src/ExportRange.cpp
    src/Exporter.cpp
    src/FrameDiff.cpp
    src/TimelineFrameRenderer.cpp
    src/SmartRender.h
    src/SmartRender.cpp
    src/TextOverlayBake.h
    src/TextOverlayBake.cpp
    src/TrackMatteBake.h
    src/TrackMatteBake.cpp
    src/ClipGeometry.h
    src/ClipGeometry.cpp
    src/LayerStyle.h
    src/LayerStyle.cpp
    src/clipanim/ClipAnim.h
    src/clipanim/ClipAnim.cpp
    src/TrackMatteKey.h
    src/TrackMatteKey.cpp
    src/CodecDetector.cpp
    src/UndoTrace.h
    src/UndoTrace.cpp
    src/UndoManager.cpp
    src/Overlay.cpp
    src/OverlayDialogs.cpp
    src/AutoColor.h
    src/AutoColor.cpp
    src/VideoEffect.cpp
    src/WbPick.h
    src/WbPick.cpp
    src/FractalNoise.h
    src/FractalNoise.cpp
    src/EffectParamSchema.h
    src/EffectParamSchema.cpp
    src/VideoEffectDialogs.cpp
    src/ColorGradingPanel.cpp
    src/ColorWheelWidget.cpp
    src/CredentialDialog.cpp
    src/CredentialVault.h
    src/CredentialVault.cpp
    src/CredentialAuditLog.h
    src/CredentialAuditLog.cpp
    src/PremiereXmlExporter.h
    src/PremiereXmlExporter.cpp
    src/YtdlpDownloadDialog.h
    src/YtdlpDownloadDialog.cpp
    src/YtdlpDownloader.h
    src/YtdlpDownloader.cpp
    src/YoutubeChapterGen.h
    src/YoutubeChapterGen.cpp
    src/selftests/ytdlp_downloader_selftest.cpp
    src/selftests/premiere_xml_selftest.cpp
    src/selftests/youtube_chapter_selftest.cpp
    src/selftests/auto_clip_gen_selftest.cpp
    src/selftests/command_search_selftest.cpp
    src/selftests/import_ingest_selftest.cpp
    src/selftests/media_pool_selftest.cpp
    src/selftests/three_point_edit_selftest.cpp
    src/selftests/trim_ops_selftest.cpp
    src/CommandSearch.h
    src/CommandSearch.cpp
    src/MediaPool.h
    src/MediaPool.cpp
    src/MediaPoolDock.cpp
    # SM (ソースモニター3点編集): 純粋エンジン + ドック (SM-2 で本体実装)。
    src/ThreePointEdit.h
    src/ThreePointEdit.cpp
    # TR (トリムツール): Ripple/Roll/Slip/Slide の純粋計算エンジン。
    src/TrimOps.h
    src/TrimOps.cpp
    # AB (オーディオバス): Fairlight 風バス/サブミックス/AUXセンドの純粋エンジン。
    src/AudioBusRouting.h
    src/AudioBusRouting.cpp
    src/playback/AudioPan.h
    src/playback/smartrender_flag.h
    src/SilenceCut.cpp
    src/BeatDetect.cpp
    src/AudioBusPanel.cpp
    src/selftests/audio_bus_selftest.cpp
    src/selftests/StereoPanSelftest.cpp
    src/selftests/ExportRangeSelftest.cpp
    src/selftests/CapcutCaptionSelftest.cpp
    src/selftests/AutoColorSelftest.cpp
    src/selftests/LayerStyleSelftest.cpp
    src/selftests/LayerStyleUiSelftest.cpp
    src/selftests/RightclickPausePrefSelftest.cpp
    src/selftests/WbEyedropperSelftest.cpp
    src/selftests/SubtitleCpsSelftest.cpp
    src/selftests/RgbParadeSelftest.cpp
    src/selftests/atempo_resolve_selftest.cpp
    src/selftests/silence_cut_selftest.cpp
    src/selftests/beat_detect_selftest.cpp
    src/selftests/safe_zone_selftest.cpp
    src/selftests/onion_skin_selftests.cpp
    src/SafeZone.h
    src/SafeZone.cpp
    src/OnionSkin.h
    src/OnionSkin.cpp
    src/selftests/subtitle_karaoke_selftest.cpp
    # SP (スペクトル編集): iZotope RX 風 STFT/iSTFT 純粋エンジン + スペクトログラム UI。
    src/SpectralEngine.h
    src/SpectralEngine.cpp
    src/SpectrogramWidget.cpp
    src/SpectralEditDialog.cpp
    src/selftests/spectral_edit_selftest.cpp
    # AC (ACES シーンリファード色管理): 自前行列 ACES 中核変換 + 色管理 Dialog。
    src/AcesColor.h
    src/AcesColor.cpp
    src/ColorManagementDialog.cpp
    src/selftests/aces_color_selftest.cpp
    # DV (Dolby Vision): PQ ST.2084 + 動的メタデータモデル + Dolby Vision XML 生成 + Dialog。
    src/DolbyVisionMetadata.h
    src/DolbyVisionMetadata.cpp
    src/color/DvTimelineBuilder.h
    src/color/DvTimelineBuilder.cpp
    src/DolbyVisionDialog.cpp
    src/selftests/dolby_vision_selftest.cpp
    src/selftests/dv_timeline_selftest.cpp
    # CC (放送 CC): CEA-608/708 + SCC サイドカー純粋エンジン + 編集 Dialog。
    src/BroadcastCaption.h
    src/BroadcastCaption.cpp
    src/BroadcastCaptionDialog.cpp
    src/selftests/broadcast_caption_selftest.cpp
    # AM (AutoMatte): グリーンバック不要の自動マッティング純粋エンジン + 編集 Dialog。
    src/AutoMatte.h
    src/AutoMatte.cpp
    src/AutoMatteDialog.cpp
    src/selftests/auto_matte_selftest.cpp
    # ED (EDL エクスポート): CMX3600 EDL の純粋書き出しエンジン (放送/Avid/DaVinci 互換)。
    src/EdlExport.h
    src/EdlExport.cpp
    src/selftests/edl_export_selftest.cpp
    # WS (ワークスペース): 名前付きドックレイアウトの純粋モデル (Premiere/Resolve 相当)。
    src/WorkspaceManager.h
    src/WorkspaceManager.cpp
    src/selftests/workspace_selftest.cpp
    # TB (テキストベース編集): 文字起こし駆動編集の純粋エンジン (Premiere/Descript 相当)。
    src/TextBasedEdit.h
    src/TextBasedEdit.cpp
    src/TextBasedEditDialog.cpp
    src/selftests/text_based_edit_selftest.cpp
    # PPTX (スライド書き出し): OOXML パートツリー + store-ZIP の純粋エンジン。
    src/PptxExport.h
    src/PptxExport.cpp
    src/PptxExportDialog.cpp
    src/selftests/pptx_export_selftest.cpp
    # ASC CDL (.cc/.ccc/.cdl 書き出し): ASC CDL v1.01 SOP/Sat の純粋エンジン。
    src/colorexport/AscCdlExport.h
    src/colorexport/AscCdlExport.cpp
    src/color/ClipColor.h
    src/color/ClipColor.cpp
    src/color/ClipColorTransform.h
    src/color/ClipColorTransform.cpp
    src/color/ClipOdt.h
    src/color/ClipOdt.cpp
    src/color/SwsColorParams.h
    src/color/SwsColorParams.cpp
    src/AscCdlExportDialog.h
    src/AscCdlExportDialog.cpp
    src/selftests/asc_cdl_export_selftest.cpp
    src/selftests/clip_color_selftest.cpp
    src/selftests/clip_idt_selftest.cpp
    src/selftests/clip_odt_selftest.cpp
    src/selftests/sws_color_selftest.cpp
    # Playback quality/cache (マルチトラック再生強化): CompositeFrameCache + PlaybackQualityPolicy。
    src/playback/CompositeFrameCache.h
    src/playback/CompositeFrameCache.cpp
    src/playback/PlaybackQualityPolicy.h
    src/playback/PlaybackQualityPolicy.cpp
    src/playback/SnsFit.h
    src/playback/SnsFit.cpp
    src/selftests/composite_frame_cache_selftests.cpp
    src/selftests/playback_quality_policy_selftests.cpp
    src/selftests/sns_fit_selftest.cpp
    src/selftests/clip_parent_parity_selftest.cpp
    src/playback/AutoProxyPolicy.h
    src/playback/AutoProxyPolicy.cpp
    src/selftests/auto_proxy_policy_selftests.cpp
    # EA (露出/フォーカス確認エイド): False Color / Zebra / Focus Peaking のプレビュー専用純粋エンジン。
    src/ExposureAids.h
    src/ExposureAids.cpp
    src/selftests/exposure_aids_selftests.cpp
    # GPU compositor math (Stage 1 dormant pure engine + headless selftest)。
    src/selftests/gpu_composite_flag_selftest.cpp
    src/playback/GpuCompositeMath.h
    src/playback/GpuCompositeMath.cpp
    src/selftests/gpu_composite_math_selftests.cpp
    # HDR 16-bit compositor math (dormant pure reference engine + headless selftest)。
    src/playback/HdrCompositeMath.h
    src/playback/HdrCompositeMath.cpp
    src/selftests/hdr_composite_math_selftests.cpp
    src/playback/hdrexport16_flag.h
    src/playback/motionblur_flag.h
    src/playback/hdroverlay_flag.h
    src/playback/clipidt_flag.h
    src/playback/idtgpu_flag.h
    src/playback/dvxml_flag.h
    src/playback/TlrCompose16.h
    src/playback/TlrCompose16.cpp
    # Stage10: ingest-time ColorMeta auto-populate (flag) + pixfmt bit-depth helper
    src/playback/hdringest_flag.h
    src/playback/PixFmtDepth.h
    src/playback/PixFmtDepth.cpp
    src/playback/HdrIngestProbe.cpp
    src/selftests/hdr_export16_selftest.cpp
    src/selftests/hdr_ingest_colormeta_selftest.cpp
    src/selftests/hdr_overlay_policy_selftest.cpp
    src/selftests/preview16_policy_selftest.cpp
    # Stage7: 16-bit RGBA64 track-matte compositor + parity selftest
    src/playback/hdrmatte16_flag.h
    src/playback/swsmatrix_flag.h
    src/playback/TrackMatteCompose16.h
    src/playback/TrackMatteCompose16.cpp
    src/selftests/matte16_parity_selftests.cpp
    # GPU layer compositor (Stage 2 offscreen-FBO compositor + GPU/CPU parity selftest)。
    src/playback/GpuLayerCompositor.h
    src/playback/GpuLayerCompositor.cpp
    src/selftests/gpu_composite_parity_selftests.cpp
    # Stage8: GPU 16-bit track-matte (composite16Matte) + parity
    src/selftests/gpu_composite_parity16_matte_selftest.cpp
    # Stage9: GPU IDT matte parity shares the GPU IDT parity translation unit
    src/selftests/gpu_idt_parity_selftest.cpp
    # GPU RGBA16 (16-bit HDR) compositor parity selftest (Stage4C HDR-2: composite16 vs HdrCompositeMath oracle)。
    src/selftests/hdr_composite_parity_selftests.cpp
    # Live track-matte index resolver (STAGE4B data plumbing + headless selftest)。
    src/playback/LiveMatteResolve.h
    src/playback/LiveMatteResolve.cpp
    src/selftests/live_matte_resolve_selftests.cpp
    src/SourceMonitorDock.cpp
    src/MarkerPanelDock.cpp
    src/CommandPaletteDialog.h
    src/CommandPaletteDialog.cpp
    src/CredentialStore.cpp
    src/EffectControlsPanel.h
    src/EffectControlsPanel.cpp
    src/GraphEditorPanel.h
    src/GraphEditorPanel.cpp
    src/selftests/GraphEditorSelftest.cpp
    src/MotionSectionWidget.h
    src/MotionSectionWidget.cpp
    src/MotionPreset.h
    src/MotionPreset.cpp
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
    src/SubtitleKaraoke.h
    src/SubtitleKaraoke.cpp
    src/SubtitleTrackRenderer.h
    src/SubtitleTrackRenderer.cpp
    src/MagneticTimeline.h
    src/MagneticTimeline.cpp
    src/CaptionTrack.h
    src/CaptionTrack.cpp
    src/CaptionCps.h
    src/CaptionStyle.h
    src/CaptionStyle.cpp
    src/SubtitleIO.h
    src/SubtitleIO.cpp
    src/CaptionEditorDialog.h
    src/CaptionEditorDialog.cpp
    src/TranscriptHighlighter.h
    src/TranscriptHighlighter.cpp
    src/TranscriptHighlightDialog.h
    src/TranscriptHighlightDialog.cpp
    src/SpeechRecognizer.h
    src/SpeechRecognizer.cpp
    src/WhisperTranscriber.h
    src/WhisperTranscriber.cpp
    src/WhisperTranscribeDialog.h
    src/WhisperTranscribeDialog.cpp
    src/selftests/whisper_transcribe_selftest.cpp
    src/selftests/transcript_highlighter_selftest.cpp
    src/selftests/transcript_highlighter_offline_selftest.cpp
    src/MotionTracker.cpp
    src/MotionTrackerDialog.h
    src/MotionTrackerDialog.cpp
    src/MotionStabilizer.cpp
    src/TrackerLink.cpp
    src/TrackerPreset.h
    src/TrackerPreset.cpp
    src/PlanarTrackerPreset.h
    src/PlanarTrackerPreset.cpp
    src/PlanarTrackerPresetRegistry.h
    src/PlanarTrackerPresetRegistry.cpp
    src/TrackerPresetRegistry.h
    src/TrackerPresetRegistry.cpp
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
    src/PlanarTrackerDialog.cpp
    src/PlanarTrackerDialog.h
    src/ColorMatchAnalyzer.cpp
    src/ColorMatchAnalyzer.h
    src/ColorMatchLutGenerator.cpp
    src/ColorMatchLutGenerator.h
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
    src/AutoClipGenerator.h
    src/AutoClipGenerator.cpp
    src/AutoClipDialog.h
    src/AutoClipDialog.cpp
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
    src/BlenderBpyBridge.h
    src/BlenderBpyBridge.cpp
    src/BlenderExrReader.h
    src/BlenderExrReader.cpp

    # Sprint 16 — Mobile export pack (US-MOB-1 / US-MOB-2)
    src/MobileDeviceProfile.h
    src/MobileDeviceProfile.cpp
    src/MobileCodecConstraints.h
    src/MobileCodecConstraints.cpp
    src/MobileColorSpace.h
    src/MobileColorSpace.cpp
    src/MobilePreset.h
    src/MobilePreset.cpp
    src/MobileRotate.h
    src/MobileRotate.cpp
    src/MobileExportDialog.h
    src/MobileExportDialog.cpp

    # Sprint 16 — OBS recording importer (US-OBS-1)
    src/ObsScanner.h
    src/ObsScanner.cpp
    src/ObsProfile.h
    src/ObsProfile.cpp
    src/ObsLayout.h
    src/ObsLayout.cpp

    # Sprint 16 — Affinity Photo / Designer / Publisher importers (US-AFF-1)
    src/AffinityPsdImporter.h
    src/AffinityPsdImporter.cpp
    src/AffinityVectorImporter.h
    src/AffinityVectorImporter.cpp

    # Sprint 16 — Blender mesh importer (US-BLE-1)
    src/BlenderMeshImporter.h
    src/BlenderMeshImporter.cpp

    # Sprint 16 — Unified import hub (US-INT-1)
    src/ImportHubDialog.h
    src/ImportHubDialog.cpp
    src/ImportIngest.h
    src/ImportIngest.cpp

    # Sprint 18 — Collaboration (US-COL-1 / US-COL-2 / US-COL-4)
    src/CollaborationModel.h
    src/CollaborationModel.cpp
    src/CommentsDockWidget.h
    src/CommentsDockWidget.cpp
    src/CollabHistoryLog.h
    src/CollabHistoryLog.cpp
    src/CollabHistoryDialog.h
    src/CollabHistoryDialog.cpp

    # Sprint 18 — Collaboration project share (US-COL-3)
    src/CollabProjectShare.h
    src/CollabProjectShare.cpp

    # Sprint 17 — YouTube upload pipeline (US-YT-1 .. US-YT-4)
    src/YoutubeOAuth.h
    src/YoutubeOAuth.cpp
    src/YoutubeUploadClient.h
    src/YoutubeUploadClient.cpp
    src/YoutubeUploadManager.h
    src/YoutubeUploadManager.cpp
    src/YoutubeUploadDialog.h
    src/YoutubeUploadDialog.cpp

    # Sprint 19 — Auto color matching dialog (US-CMA-3)
    src/ColorMatchDialog.h
    src/ColorMatchDialog.cpp

    # Sprint 20 — Platform delivery / editorial interchange / smart edit / cloud render
    src/VimeoOAuth.h
    src/VimeoOAuth.cpp
    src/VimeoUploadClient.h
    src/VimeoUploadClient.cpp
    src/VimeoUploadManager.h
    src/VimeoUploadManager.cpp
    src/VimeoUploadDialog.h
    src/VimeoUploadDialog.cpp
    src/TwitchStreamConfig.h
    src/TwitchStreamConfig.cpp
    src/TwitchStreamDialog.h
    src/TwitchStreamDialog.cpp
    src/FrameIoImporter.h
    src/FrameIoImporter.cpp
    src/DavinciResolveXmlExporter.h
    src/DavinciResolveXmlExporter.cpp
    src/FcpxmlExporter.h
    src/FcpxmlExporter.cpp
    src/SmartEditAssistant.h
    src/SmartEditAssistant.cpp
    src/SmartEditDialog.h
    src/SmartEditDialog.cpp
    src/CloudRenderClient.h
    src/CloudRenderClient.cpp
    src/CloudRenderDialog.h
    src/CloudRenderDialog.cpp

    # Sprint 21 — X(Twitter)/Instagram delivery, project templates, loudness, HDR, multicam, batch export
    src/XVideoUpload.h
    src/XVideoUpload.cpp
    src/XVideoDialog.h
    src/XVideoDialog.cpp
    src/InstagramPublish.h
    src/InstagramPublish.cpp
    src/InstagramPublishDialog.h
    src/InstagramPublishDialog.cpp
    src/ProjectTemplate.h
    src/ProjectTemplate.cpp
    src/ProjectTemplateDialog.h
    src/ProjectTemplateDialog.cpp
    src/LoudnessMaster.h
    src/LoudnessMaster.cpp
    src/LoudnessMasterDialog.h
    src/LoudnessMasterDialog.cpp
    src/HdrGrading.h
    src/HdrGrading.cpp
    src/HdrGradingDialog.h
    src/HdrGradingDialog.cpp
    src/MultiCamSync.h
    src/MultiCamSync.cpp
    src/MultiCamSyncDialog.h
    src/MultiCamSyncDialog.cpp
    src/BatchExportQueue.h
    src/BatchExportQueue.cpp
    src/BatchExportDialog.h
    src/BatchExportDialog.cpp

    # Sprint 22 — chroma-key refine, audio restoration, animated export, easing editor, subtitle translate, lower-thirds, watermark
    src/ChromaKeyRefine.h
    src/ChromaKeyRefine.cpp
    src/ChromaKeyRefineDialog.h
    src/ChromaKeyRefineDialog.cpp
    src/AudioRestoration.h
    src/AudioRestoration.cpp
    src/AudioRestorationDialog.h
    src/AudioRestorationDialog.cpp
    src/AnimatedExport.h
    src/AnimatedExport.cpp
    src/AnimatedExportDialog.h
    src/AnimatedExportDialog.cpp
    src/EasingCurveModel.h
    src/EasingCurveModel.cpp
    src/EasingCurveEditorDialog.h
    src/EasingCurveEditorDialog.cpp
    src/SubtitleTranslator.h
    src/SubtitleTranslator.cpp
    src/SubtitleTranslatorDialog.h
    src/SubtitleTranslatorDialog.cpp
    src/LowerThirdTemplates.h
    src/LowerThirdTemplates.cpp
    src/LowerThirdDialog.h
    src/LowerThirdDialog.cpp
    src/WatermarkOverlay.h
    src/WatermarkOverlay.cpp
    src/WatermarkDialog.h
    src/WatermarkDialog.cpp

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

# Sprint 16 / US-AFF-1 / US-BLE-1: optional Qt6::Svg / Qt6::Pdf / assimp links.
# All three are guarded so the build works whether or not they are installed.
if(TARGET Qt6::Svg)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Svg)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VEDITOR_HAVE_QT_SVG=1)
endif()
if(TARGET Qt6::Pdf)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Pdf)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VEDITOR_HAVE_QT_PDF=1)
endif()
if(TARGET assimp::assimp)
    target_link_libraries(${PROJECT_NAME} PRIVATE assimp::assimp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VEDITOR_HAVE_ASSIMP=1)
endif()

if(WIN32)
    # opengl32 — needed for wglGetCurrentDC and the WGL_NV_DX_interop2 entry
    # points (Phase 1e). Qt6::OpenGL is the Qt wrapper, not the system GL.
    target_link_libraries(${PROJECT_NAME} PRIVATE dbghelp opengl32 advapi32)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    VEDITOR_EDITION="${VEDITOR_EDITION}"
    VEDITOR_LIBAVCORE_WITH_QIMAGE=1
    $<$<BOOL:${VEDITOR_PLANAR_TRACKER_SELFTEST}>:VEDITOR_PLANAR_TRACKER_SELFTEST=1>
    $<$<BOOL:${VEDITOR_EFFECT_PARAM_SCHEMA_SELFTEST}>:VEDITOR_EFFECT_PARAM_SCHEMA_SELFTEST=1>
    $<$<BOOL:${VEDITOR_BRUSH_SELFTEST}>:VEDITOR_BRUSH_SELFTEST=1>
    $<$<BOOL:${VEDITOR_SNSPACK_SELFTEST}>:VEDITOR_SNSPACK_SELFTEST=1>
    $<$<BOOL:${VEDITOR_NODEGRAPH_SELFTEST}>:VEDITOR_NODEGRAPH_SELFTEST=1>
    $<$<STREQUAL:${VEDITOR_EDITION},modern>:VEDITOR_AV1=1>
)
