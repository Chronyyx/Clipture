cmake_minimum_required(VERSION 3.24)
project(clipture_engine LANGUAGES C CXX)

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

add_executable(clipture_engine
  src/main.cpp
  src/AudioCaptureWorker.cpp
  src/CaptureSession.cpp
  src/Diagnostics.cpp
  src/EncoderWorker.cpp
  src/Engine.cpp
  src/FrameQueue.cpp
  src/Mp4Muxer.cpp
  src/NativeRuntime.cpp
  src/Tonemapper.cpp
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

target_link_libraries(clipture_engine PRIVATE dxgi d3d11 d3dcompiler windowsapp runtimeobject ole32 avrt mfplat mfreadwrite mfuuid winmm)

