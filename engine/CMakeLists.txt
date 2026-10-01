cmake_minimum_required(VERSION 3.24)
project(clipture_engine LANGUAGES C CXX)

# Sidecars must run on a clean Windows installation without a separate VC++
# redistributable. Apply to both the engine and its native test executable.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)

add_library(clipture_selection_trace STATIC src/FrameSelectionTrace.cpp)
target_include_directories(clipture_selection_trace PUBLIC include)
target_compile_features(clipture_selection_trace PUBLIC cxx_std_20)
target_compile_definitions(clipture_selection_trace PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_library(clipture_json_fields STATIC src/protocol/JsonFields.cpp)
target_include_directories(clipture_json_fields PUBLIC include)
target_compile_features(clipture_json_fields PUBLIC cxx_std_20)

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

add_executable(clipture_engine
  src/main.cpp
  src/AacEncoderSession.cpp
  src/AudioCaptureWorker.cpp
  src/AudioReplayCoordinator.cpp
  src/CaptureBackend.cpp
  src/CaptureSession.cpp
  src/DesktopDuplicationBackend.cpp
  src/DesktopFramePreparation.cpp
  src/DesktopPointerCompositor.cpp
  src/DesktopPointerShape.cpp
  src/Diagnostics.cpp
  src/EncoderWorker.cpp
  src/NvencRateControl.cpp
  src/Engine.cpp
  src/FrameQueue.cpp
  src/GpuFrameHandoff.cpp
  src/SharedTextureReader.cpp
  src/EncoderSourceSnapshot.cpp
  src/H264PacketAnalyzer.cpp
  src/Mp4Muxer.cpp
  src/NativeRuntime.cpp
  src/ReplaySegmentStore.cpp
  src/replay/Mp4SamplePacker.cpp
  src/replay/PayloadLayoutPlan.cpp
  src/replay/CopyPayloadWriter.cpp
  src/replay/PayloadCloneJob.cpp
  src/platform/windows/CloneFileInfo.cpp
  src/platform/windows/SealedPayloadFile.cpp
  src/platform/windows/WindowsPayloadOutput.cpp
  src/mux/PreparedVideoLayout.cpp
  src/mux/InPlaceMp4Header.cpp
  src/replay/InPlaceMediaBuffer.cpp
  src/replay/InPlacePacketArchive.cpp
  src/replay/ConsumedWindow.cpp
  src/replay/BackfillPace.cpp
  src/platform/windows/InPlaceFile.cpp
  src/RawInputHotkey.cpp
  src/Tonemapper.cpp
  src/WgcCaptureBackend.cpp
  src/WgcFramePreparation.cpp
  third_party/rnnoise/src/celt_lpc.c
  third_party/rnnoise/src/denoise.c
  third_party/rnnoise/src/kiss_fft.c
  third_party/rnnoise/src/pitch.c
  third_party/rnnoise/src/rnn.c
  third_party/rnnoise/src/nnet.c
  third_party/rnnoise/src/nnet_default.c
  third_party/rnnoise/src/parse_lpcnet_weights.c
  third_party/rnnoise/src/rnnoise_tables.c
  third_party/rnnoise/src/rnnoise_data.c
)

target_include_directories(clipture_engine PRIVATE include ../third_party/nv-codec-headers/include third_party/rnnoise/include)
target_compile_features(clipture_engine PRIVATE cxx_std_20)
target_compile_definitions(clipture_engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
  target_compile_options(clipture_engine PRIVATE /W4 /permissive-)
endif()

target_link_libraries(clipture_engine PRIVATE clipture_selection_trace clipture_json_fields dxgi d3d11 d3dcompiler windowsapp runtimeobject ole32 avrt mfplat mfreadwrite mfuuid winmm user32)

if(BUILD_TESTING)
  add_executable(clipture_selection_trace_tests tests/FrameSelectionTraceTests.cpp)
  target_link_libraries(clipture_selection_trace_tests PRIVATE clipture_selection_trace)
  add_test(NAME clipture_selection_trace COMMAND clipture_selection_trace_tests)
  add_executable(clipture_rate_control_tests tests/NvencRateControlTests.cpp src/NvencRateControl.cpp)
  target_include_directories(clipture_rate_control_tests PRIVATE include ../third_party/nv-codec-headers/include)
  target_compile_features(clipture_rate_control_tests PRIVATE cxx_std_20)
  add_test(NAME clipture_rate_control COMMAND clipture_rate_control_tests)
  add_executable(clipture_encoded_frontier_tests tests/EncodedFrontierTests.cpp)
  target_include_directories(clipture_encoded_frontier_tests PRIVATE include)
  target_compile_features(clipture_encoded_frontier_tests PRIVATE cxx_std_20)
  add_test(NAME clipture_encoded_frontier COMMAND clipture_encoded_frontier_tests)
  add_executable(clipture_json_fields_tests tests/JsonFieldsTests.cpp)
  target_link_libraries(clipture_json_fields_tests PRIVATE clipture_json_fields)
  add_test(NAME clipture_json_fields COMMAND clipture_json_fields_tests)
  add_executable(clipture_fresh_conversion_tests tests/FreshConversionTests.cpp)
  target_include_directories(clipture_fresh_conversion_tests PRIVATE include src)
  target_compile_features(clipture_fresh_conversion_tests PRIVATE cxx_std_20)
  target_compile_definitions(clipture_fresh_conversion_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(clipture_fresh_conversion_tests PRIVATE d3d11 dxgi)
  add_test(NAME clipture_fresh_conversion_policy COMMAND clipture_fresh_conversion_tests)
  add_test(NAME clipture_fresh_conversion_pixels COMMAND clipture_fresh_conversion_tests --gpu)
  set_tests_properties(clipture_fresh_conversion_pixels PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
  add_executable(clipture_source_snapshot_tests tests/EncoderSourceSnapshotTests.cpp
    src/EncoderSourceSnapshot.cpp src/SharedTextureReader.cpp)
  target_include_directories(clipture_source_snapshot_tests PRIVATE src include)
  target_compile_features(clipture_source_snapshot_tests PRIVATE cxx_std_20)
  target_compile_definitions(clipture_source_snapshot_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(clipture_source_snapshot_tests PRIVATE d3d11 dxgi)
  add_test(NAME clipture_encoder_source_snapshot COMMAND clipture_source_snapshot_tests)
  set_tests_properties(clipture_encoder_source_snapshot PROPERTIES TIMEOUT 20)
  add_executable(clipture_source_timestamp_tests tests/SourceTimestampGateTests.cpp)
  target_include_directories(clipture_source_timestamp_tests PRIVATE include)
  target_compile_features(clipture_source_timestamp_tests PRIVATE cxx_std_20)
  add_test(NAME clipture_source_timestamp COMMAND clipture_source_timestamp_tests)
  add_executable(clipture_shared_reader_tests tests/SharedTextureReaderTests.cpp
    src/SharedTextureReader.cpp src/CaptureBackend.cpp src/FrameQueue.cpp)
  target_include_directories(clipture_shared_reader_tests PRIVATE src include)
  target_compile_features(clipture_shared_reader_tests PRIVATE cxx_std_20)
  target_compile_definitions(clipture_shared_reader_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(clipture_shared_reader_tests PRIVATE clipture_selection_trace d3d11 dxgi user32)
  add_test(NAME clipture_shared_texture_reader COMMAND clipture_shared_reader_tests)
  set_tests_properties(clipture_shared_texture_reader PROPERTIES TIMEOUT 20)
  add_executable(clipture_gpu_probe_tests tests/GpuStageProbeTests.cpp)
  target_include_directories(clipture_gpu_probe_tests PRIVATE src include)
  target_compile_features(clipture_gpu_probe_tests PRIVATE cxx_std_20)
  target_compile_definitions(clipture_gpu_probe_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(clipture_gpu_probe_tests PRIVATE d3d11)
  add_test(NAME clipture_gpu_probe_enabled COMMAND clipture_gpu_probe_tests)
  set_tests_properties(clipture_gpu_probe_enabled PROPERTIES ENVIRONMENT "CLIPTURE_PIPELINE_TRACE=1" TIMEOUT 15)
  add_test(NAME clipture_gpu_probe_disabled COMMAND clipture_gpu_probe_tests)
  set_tests_properties(clipture_gpu_probe_disabled PROPERTIES ENVIRONMENT "CLIPTURE_PIPELINE_TRACE=0" TIMEOUT 15)
  add_executable(clipture_gpu_handoff_tests tests/GpuFrameHandoffTests.cpp src/GpuFrameHandoff.cpp)
  target_include_directories(clipture_gpu_handoff_tests PRIVATE src include)
  target_compile_features(clipture_gpu_handoff_tests PRIVATE cxx_std_20)
  target_compile_definitions(clipture_gpu_handoff_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(clipture_gpu_handoff_tests PRIVATE d3d11 dxgi)
  add_test(NAME clipture_gpu_frame_handoff COMMAND clipture_gpu_handoff_tests)
  set_tests_properties(clipture_gpu_frame_handoff PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
  add_executable(clipture_preparation_gpu_tests
    tests/DesktopPreparationGpuTests.cpp
    src/DesktopFramePreparation.cpp src/WgcFramePreparation.cpp src/DesktopPointerCompositor.cpp
    src/DesktopPointerShape.cpp src/Tonemapper.cpp src/CaptureBackend.cpp src/FrameQueue.cpp)
  target_include_directories(clipture_preparation_gpu_tests PRIVATE include src)
  target_compile_features(clipture_preparation_gpu_tests PRIVATE cxx_std_20)
  target_compile_definitions(clipture_preparation_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(clipture_preparation_gpu_tests PRIVATE clipture_selection_trace d3d11 dxgi d3dcompiler user32)
  add_test(NAME clipture_desktop_preparation_pixels COMMAND clipture_preparation_gpu_tests)
  add_test(NAME clipture_shared_hdr_pixels COMMAND clipture_preparation_gpu_tests --shared)
  add_executable(clipture_preparation_tests tests/DeferredFramePreparationTests.cpp)
  target_include_directories(clipture_preparation_tests PRIVATE include)
  target_compile_features(clipture_preparation_tests PRIVATE cxx_std_20)
  add_test(NAME clipture_deferred_preparation COMMAND clipture_preparation_tests)
  add_executable(clipture_cadence_tests tests/EncoderCadenceTests.cpp)
  target_include_directories(clipture_cadence_tests PRIVATE include)
  target_compile_features(clipture_cadence_tests PRIVATE cxx_std_20)
  add_test(NAME clipture_encoder_cadence COMMAND clipture_cadence_tests)
  add_executable(clipture_pcm_block_mixer_tests tests/audio/PcmBlockMixerTests.cpp)
  target_include_directories(clipture_pcm_block_mixer_tests PRIVATE include)
  target_compile_features(clipture_pcm_block_mixer_tests PRIVATE cxx_std_20)
  add_test(NAME clipture_pcm_block_mixer COMMAND clipture_pcm_block_mixer_tests)
  add_executable(clipture_loopback_silence_tests tests/audio/LoopbackSilenceTests.cpp)
  target_include_directories(clipture_loopback_silence_tests PRIVATE include)
  target_compile_features(clipture_loopback_silence_tests PRIVATE cxx_std_20)
  add_test(NAME clipture_loopback_silence COMMAND clipture_loopback_silence_tests)
  add_executable(clipture_packet_tests
    tests/PacketArchitectureTests.cpp
    tests/audio/AudioMixerIntegrationTests.cpp
    src/AacEncoderSession.cpp
    src/AudioReplayCoordinator.cpp
    src/DesktopPointerShape.cpp
    src/H264PacketAnalyzer.cpp
    src/FrameQueue.cpp
    src/Mp4Muxer.cpp
    src/ReplaySegmentStore.cpp
    src/replay/Mp4SamplePacker.cpp
    src/replay/PayloadLayoutPlan.cpp
    src/mux/PreparedVideoLayout.cpp
    src/mux/InPlaceMp4Header.cpp
    src/replay/InPlaceMediaBuffer.cpp
    src/replay/InPlacePacketArchive.cpp
    src/replay/ConsumedWindow.cpp
    src/replay/BackfillPace.cpp
    src/platform/windows/InPlaceFile.cpp
  )
  target_include_directories(clipture_packet_tests PRIVATE include)
  target_compile_features(clipture_packet_tests PRIVATE cxx_std_20)
  target_compile_definitions(clipture_packet_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(clipture_packet_tests PRIVATE clipture_selection_trace ole32 mfplat mfreadwrite mfuuid winmm)
  if(MSVC)
    target_compile_options(clipture_packet_tests PRIVATE /W4 /permissive-)
  endif()
  add_test(NAME clipture_packet_architecture COMMAND clipture_packet_tests)
  add_subdirectory(tests/replay)
endif()
