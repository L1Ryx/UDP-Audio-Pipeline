# Low-Latency UDP Audio Pipeline

C++20/CMake starter for a LAN-focused real-time audio streaming engine.

The project is intentionally scoped for a portfolio-grade build:

- Custom 16-byte UDP packet header for PCM audio frames.
- Lock-free SPSC queue for network-to-audio handoff.
- Adaptive jitter buffer as the receiver-side latency control point.
- Packet loss concealment as a first-class subsystem.
- AVX2 DSP kernels for gain, downmixing, peak detection, and limiting on x86/x64.
- LAN and loopback latency metrics rather than Internet-scale claims.

## Current Scope

This repo starts with the spine of the engine, not the finished engine:

- `udp_audio_core`: reusable C++20 library.
- `udp_audio_smoke`: tiny executable for sanity checks while developing.
- `udp_audio_tests`: assert-based smoke tests with no external dependencies.

PLC begins as a low-latency hold-and-decay concealer. That is deliberately modest and
easy to benchmark; later iterations can replace it with pitch-period/WSOLA concealment
without changing the public module boundary.

## Build

In CLion, open this folder and load the CMake project. The included presets are
set up for Debug/Release builds and tests.

From a terminal, these commands assume `cmake` and `ninja` are on your `PATH`:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

On this Mac, CLion's bundled CMake/Ninja were used to validate the build because the
command-line tools were not on `PATH`.

## Target Platform

The main target is x86/x64 Linux for latency testing and AVX2 benchmarking. The project
also configures on non-x86 hosts, but AVX2 sources are skipped there so CLion can still
index and build the scalar path.

See `docs/PROJECT_BRIEF.md` for the trimmed project scope and milestone plan.

