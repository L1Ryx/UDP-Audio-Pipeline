# Lossy Audio Lab

Lossy Audio Lab is a desktop app for hearing how audio changes when packets are
lost, delayed, or recovered in a real-time-style audio pipeline.

Load a WAV or MP3, choose a network preset, press Run, and compare the result with
waveform, loudness, recovery timeline, and latency metrics.

## What It Does

- Loads WAV and MP3 files.
- Simulates independent and burst packet loss.
- Adds configurable jitter and playout buffering.
- Recovers missing audio with Opus redundancy, FEC, and PLC paths.
- Shows waveform, frame RMS, recovery status, and runtime metrics.
- Includes diagnostic sine/chirp sources for repeatable stress tests.

The presets are controlled listening scenarios, not exact models of a specific
router, carrier, or conferencing app. They are meant to make loss and recovery
tradeoffs easy to hear and compare.

## Download

Prebuilt packages are available from GitHub Releases:

- `Lossy-Audio-Lab-macOS-arm64.zip`
- `Lossy-Audio-Lab-Windows-x64.zip`

Unzip the package and run the `lossy_audio_lab` executable.

On macOS, the binary is currently unsigned. If macOS blocks the first launch,
right-click the executable and choose Open, or allow it from System Settings.

## Build From Source

Requirements:

- CMake 3.24+
- C++20 compiler
- Opus
- SDL3
- OpenGL
- Ninja, recommended

From the repository root:

```sh
cmake --preset release
cmake --build --preset release --target lossy_audio_lab
./build/release/lossy_audio_lab
```

For CLion, open the repository folder, select the `lossy_audio_lab` target, and
build/run it from the IDE.

## Project Notes

The app is built around 48 kHz mono float PCM, 10 ms audio frames, Opus encode /
decode, a jitter-buffered playout model, and measurable recovery behavior. The
command-line tools and tests in the repository are development aids for validating
those pieces, while the GUI is the user-facing release target.

More technical notes live in `docs/`.

## Release Builds

GitHub Actions packages macOS and Windows zips when a version tag such as `v0.1.0`
is pushed. See `docs/RELEASE.md` for the release checklist.
