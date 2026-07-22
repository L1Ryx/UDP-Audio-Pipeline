# Low-Latency UDP Audio Pipeline

A C++20 audio streaming engine built around custom UDP packets, jitter buffering,
packet loss concealment, and AVX2 DSP.

The first target is simple and measurable: send 48 kHz float PCM across a LAN or local
loopback with clear telemetry for latency, jitter, packet loss, and DSP cost.

## Build

Open this folder in CLion, or build from a terminal:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Current Pieces

- 10 ms mono audio frames: 480 float samples at 48 kHz.
- 16-byte UDP packet header.
- POSIX UDP socket wrapper.
- Local UDP loopback demo with fixed jitter-buffer playout telemetry.
- Lock-free SPSC queue.
- Starter jitter buffer, PLC, scalar DSP, and AVX2 DSP paths.

## Run The Loopback Demo

```sh
./build/debug/udp_audio_loopback 100
```

Optional arguments:

```sh
./build/debug/udp_audio_loopback [frames] [loss_percent] [jitter_ms] [seed]
```

Example with deterministic impairment:

```sh
./build/debug/udp_audio_loopback 50 10 20 1337
```
