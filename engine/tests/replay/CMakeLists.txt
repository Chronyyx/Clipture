add_executable(clipture_replay_tests
  main.cpp
  Mp4SamplePackerTests.cpp
  Mp4ReplayStoreTests.cpp
  FixtureParity.cpp
  InPlaceTests.cpp
  InPlaceAudioFixture.cpp
  InPlaceRollingFixture.cpp
  InPlaceArchiveTests.cpp
  PayloadLayoutTests.cpp
  PayloadCopyTests.cpp
  ExtentStoreTests.cpp
  ExtentFixtureParity.cpp
  AlignedLayoutTests.cpp
  ClonePolicyTests.cpp
  CloneJobTests.cpp
  WindowsCloneTests.cpp
  NativeCloneRetryTests.cpp
  ../../src/replay/Mp4SamplePacker.cpp
  ../../src/replay/PayloadLayoutPlan.cpp
  ../../src/replay/CopyPayloadWriter.cpp
  ../../src/replay/PayloadCloneJob.cpp
  ../../src/platform/windows/CloneFileInfo.cpp
  ../../src/platform/windows/SealedPayloadFile.cpp
  ../../src/platform/windows/WindowsPayloadOutput.cpp
  ../../src/mux/PreparedVideoLayout.cpp
  ../../src/mux/InPlaceMp4Header.cpp
  ../../src/replay/InPlaceMediaBuffer.cpp
  ../../src/replay/InPlacePacketArchive.cpp
  ../../src/replay/ConsumedWindow.cpp
  ../../src/platform/windows/InPlaceFile.cpp
  ../../src/H264PacketAnalyzer.cpp
  ../../src/ReplaySegmentStore.cpp
  ../../src/Mp4Muxer.cpp
  ../../src/AacEncoderSession.cpp
)
target_include_directories(clipture_replay_tests PRIVATE ../../include)
target_compile_features(clipture_replay_tests PRIVATE cxx_std_20)
target_compile_definitions(clipture_replay_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(clipture_replay_tests PRIVATE ole32 mfplat mfreadwrite mfuuid winmm)
if(MSVC)
  target_compile_options(clipture_replay_tests PRIVATE /W4 /permissive-)
endif()
add_test(NAME clipture_mp4_ready_replay COMMAND clipture_replay_tests)
