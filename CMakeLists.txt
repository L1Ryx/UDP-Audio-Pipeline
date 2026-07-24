cmake_minimum_required(VERSION 3.24)

project(
  lossy_audio_lab
  VERSION 0.1.0
  DESCRIPTION "Interactive C++ audio lab for packet loss, jitter, Opus recovery, and DSP profiling"
  LANGUAGES C CXX)

option(UDP_AUDIO_BUILD_TESTS "Build smoke tests" ON)
option(UDP_AUDIO_ENABLE_AVX2 "Enable AVX2 DSP kernels when building on x86/x64" ON)
option(UDP_AUDIO_ENABLE_NEON "Enable NEON DSP kernels when building on ARM64/AArch64" ON)
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
  src/audio/wav_writer.cpp
  src/audio/source.cpp
  src/codec/opus_packet.cpp
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

if(WIN32)
  target_link_libraries(udp_audio_core PUBLIC ws2_32)
endif()

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

if(UDP_AUDIO_ENABLE_NEON AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64|AARCH64)$")
  target_sources(udp_audio_core PRIVATE src/dsp/gain_neon.cpp)
  target_compile_definitions(udp_audio_core PRIVATE UDP_AUDIO_HAS_NEON=1)
  message(STATUS "NEON kernels enabled for processor: ${CMAKE_SYSTEM_PROCESSOR}")
else()
  message(STATUS "NEON kernels disabled for processor: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

add_executable(udp_audio_smoke apps/udp_audio_smoke.cpp)
target_link_libraries(udp_audio_smoke PRIVATE udp_audio_core)
udp_audio_enable_warnings(udp_audio_smoke)

add_executable(udp_audio_dsp_bench apps/udp_audio_dsp_bench.cpp)
target_link_libraries(udp_audio_dsp_bench PRIVATE udp_audio_core)
udp_audio_enable_warnings(udp_audio_dsp_bench)

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
  find_package(SDL3 CONFIG QUIET
    PATHS /opt/homebrew/lib/cmake/SDL3 /usr/local/lib/cmake/SDL3)
  find_package(OpenGL QUIET)

  if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY)
    add_library(Opus::opus UNKNOWN IMPORTED)
    set_target_properties(Opus::opus PROPERTIES
      IMPORTED_LOCATION "${OPUS_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${OPUS_INCLUDE_DIR}")

    add_executable(udp_audio_opus_loopback apps/udp_audio_opus_loopback.cpp)
    target_link_libraries(udp_audio_opus_loopback PRIVATE udp_audio_core miniaudio Opus::opus)
    udp_audio_enable_warnings(udp_audio_opus_loopback)

    add_library(udp_audio_opus_sim STATIC
      src/sim/opus_playout_sim.cpp)
    target_link_libraries(udp_audio_opus_sim PUBLIC udp_audio_core Opus::opus)
    target_compile_features(udp_audio_opus_sim PUBLIC cxx_std_20)
    udp_audio_enable_warnings(udp_audio_opus_sim)

    if(UDP_AUDIO_BUILD_TESTS)
      add_executable(udp_audio_opus_tests tests/test_opus_sim.cpp)
      target_link_libraries(udp_audio_opus_tests PRIVATE udp_audio_opus_sim)
      udp_audio_enable_warnings(udp_audio_opus_tests)
      add_test(NAME udp_audio_opus_tests COMMAND udp_audio_opus_tests)
    endif()

    if(SDL3_FOUND AND OpenGL_FOUND)
      set(UDP_AUDIO_IMGUI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")
      if(NOT EXISTS "${UDP_AUDIO_IMGUI_SOURCE_DIR}/imgui.cpp")
        include(FetchContent)
        FetchContent_Declare(dear_imgui
          GIT_REPOSITORY https://github.com/ocornut/imgui.git
          GIT_TAG v1.92.1
          SOURCE_SUBDIR cmake-do-not-add)
        FetchContent_MakeAvailable(dear_imgui)
        FetchContent_GetProperties(dear_imgui SOURCE_DIR dear_imgui_SOURCE_DIR)
        set(UDP_AUDIO_IMGUI_SOURCE_DIR "${dear_imgui_SOURCE_DIR}")
      endif()

      if(EXISTS "${UDP_AUDIO_IMGUI_SOURCE_DIR}/imgui.cpp")
        add_library(imgui_sdl3_opengl STATIC
          "${UDP_AUDIO_IMGUI_SOURCE_DIR}/imgui.cpp"
          "${UDP_AUDIO_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
          "${UDP_AUDIO_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
          "${UDP_AUDIO_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
          "${UDP_AUDIO_IMGUI_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
          "${UDP_AUDIO_IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
        target_include_directories(imgui_sdl3_opengl
          SYSTEM
          PUBLIC
            "${UDP_AUDIO_IMGUI_SOURCE_DIR}"
            "${UDP_AUDIO_IMGUI_SOURCE_DIR}/backends")
        target_link_libraries(imgui_sdl3_opengl PUBLIC SDL3::SDL3 OpenGL::GL)
        target_compile_features(imgui_sdl3_opengl PUBLIC cxx_std_20)

        add_executable(lossy_audio_lab apps/lossy_audio_lab.cpp)
        target_link_libraries(lossy_audio_lab
          PRIVATE
            udp_audio_opus_sim
            miniaudio
            imgui_sdl3_opengl)
        udp_audio_enable_warnings(lossy_audio_lab)

        install(TARGETS lossy_audio_lab
          RUNTIME DESTINATION .
          BUNDLE DESTINATION .)
        install(FILES README.md DESTINATION .)
        install(DIRECTORY docs DESTINATION . FILES_MATCHING PATTERN "*.md")
      else()
        message(STATUS "Dear ImGui not found; lossy_audio_lab will not be built")
      endif()
    else()
      message(STATUS "SDL3/OpenGL not found; lossy_audio_lab will not be built")
    endif()
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
    tests/test_audio_io.cpp
    tests/test_dsp.cpp
    tests/test_audio_frame.cpp
    tests/test_fixed_jitter_buffer.cpp
    tests/test_opus_packet.cpp
    tests/test_packet.cpp
    tests/test_plc.cpp
    tests/test_spsc_ring_buffer.cpp
    tests/test_udp_socket.cpp)
  target_link_libraries(udp_audio_tests PRIVATE udp_audio_core)
  udp_audio_enable_warnings(udp_audio_tests)
  add_test(NAME udp_audio_tests COMMAND udp_audio_tests)
endif()
