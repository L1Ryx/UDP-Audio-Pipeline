cmake_minimum_required(VERSION 3.24)

project(
  udp_audio_pipeline
  VERSION 0.1.0
  DESCRIPTION "Low-latency UDP audio pipeline with jitter buffering, PLC, and AVX2 DSP"
  LANGUAGES C CXX)

option(UDP_AUDIO_BUILD_TESTS "Build smoke tests" ON)
option(UDP_AUDIO_ENABLE_AVX2 "Enable AVX2 DSP kernels when building on x86/x64" ON)
option(UDP_AUDIO_ENABLE_OPUS "Build Opus codec/PLC baseline when libopus is available" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

add_library(miniaudio STATIC external/miniaudio/miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC external/miniaudio)
if(APPLE)
  target_link_libraries(miniaudio
    PUBLIC
      "-framework CoreAudio"
      "-framework AudioToolbox"
      "-framework AudioUnit"
      "-framework CoreFoundation")
endif()

add_library(udp_audio_core
  src/audio/frame.cpp
  src/dsp/gain.cpp
  src/dsp/gain_scalar.cpp
  src/dsp/plc.cpp
  src/protocol/packet.cpp
  src/transport/udp_socket.cpp
  src/version.cpp)

target_include_directories(udp_audio_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(udp_audio_core PUBLIC cxx_std_20)
udp_audio_enable_warnings(udp_audio_core)

if(UDP_AUDIO_ENABLE_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
  target_sources(udp_audio_core PRIVATE src/dsp/gain_avx2.cpp)
  target_compile_definitions(udp_audio_core PRIVATE UDP_AUDIO_HAS_AVX2=1)
  if(MSVC)
    target_compile_options(udp_audio_core PRIVATE /arch:AVX2)
  else()
    set_source_files_properties(src/dsp/gain_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
  endif()
else()
  message(STATUS "AVX2 kernels disabled for processor: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

add_executable(udp_audio_smoke apps/udp_audio_smoke.cpp)
target_link_libraries(udp_audio_smoke PRIVATE udp_audio_core)
udp_audio_enable_warnings(udp_audio_smoke)

add_executable(udp_audio_loopback apps/udp_audio_loopback.cpp)
target_link_libraries(udp_audio_loopback PRIVATE udp_audio_core miniaudio)
udp_audio_enable_warnings(udp_audio_loopback)

if(UDP_AUDIO_ENABLE_OPUS)
  find_path(OPUS_INCLUDE_DIR
    NAMES opus/opus.h
    HINTS /opt/homebrew/include /usr/local/include)
  find_library(OPUS_LIBRARY
    NAMES opus
    HINTS /opt/homebrew/lib /usr/local/lib)

  if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY)
    add_library(Opus::opus UNKNOWN IMPORTED)
    set_target_properties(Opus::opus PROPERTIES
      IMPORTED_LOCATION "${OPUS_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${OPUS_INCLUDE_DIR}")

    add_executable(udp_audio_opus_loopback apps/udp_audio_opus_loopback.cpp)
    target_link_libraries(udp_audio_opus_loopback PRIVATE udp_audio_core miniaudio Opus::opus)
    udp_audio_enable_warnings(udp_audio_opus_loopback)
  else()
    message(STATUS "libopus not found; udp_audio_opus_loopback will not be built")
  endif()
endif()

add_executable(udp_audio_miniaudio_playback apps/udp_audio_miniaudio_playback.cpp)
target_link_libraries(udp_audio_miniaudio_playback PRIVATE udp_audio_core miniaudio)
udp_audio_enable_warnings(udp_audio_miniaudio_playback)

if(UDP_AUDIO_BUILD_TESTS)
  enable_testing()
  add_executable(udp_audio_tests
    tests/test_main.cpp
    tests/test_dsp.cpp
    tests/test_audio_frame.cpp
    tests/test_fixed_jitter_buffer.cpp
    tests/test_packet.cpp
    tests/test_plc.cpp
    tests/test_spsc_ring_buffer.cpp
    tests/test_udp_socket.cpp)
  target_link_libraries(udp_audio_tests PRIVATE udp_audio_core)
  udp_audio_enable_warnings(udp_audio_tests)
  add_test(NAME udp_audio_tests COMMAND udp_audio_tests)
endif()
