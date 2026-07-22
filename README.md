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
- Hold-and-decay PLC for concealed output frames during jitter-buffer underruns.
- Opus encode/decode loopback baseline using decoder-side PLC.
- miniaudio playback demo using a real audio callback.
- Lock-free SPSC queue.
- Starter jitter buffer, PLC, scalar DSP, and AVX2 DSP paths.

## Run The Loopback Demo

```sh
./build/debug/udp_audio_loopback 100
```

Optional arguments:

```sh
./build/debug/udp_audio_loopback [frames] [loss_percent] [jitter_ms] [seed] [play_audio] [record_wav] [plc_mode] [source_mode]
```

Example with deterministic impairment:

```sh
./build/debug/udp_audio_loopback 50 10 20 1337
```

Set `play_audio` to `1` to play the jitter/PLC output through miniaudio:

```sh
./build/debug/udp_audio_loopback 100 10 20 1337 1
```

Record the receiver playout stream to a 48 kHz mono float WAV:

```sh
./build/debug/udp_audio_loopback 100 10 20 1337 0 recordings/output.wav
```

PLC modes for comparison:

- `none`: missing frames become silence.
- `repeat`: repeat the previous frame with decay.
- `periodic`: estimate a whole-sample waveform period and continue it.
- `periodic_interp`: estimate a fractional waveform period and continue it.

```sh
./build/debug/udp_audio_loopback 50 20 25 1337 0 recordings/plc_periodic_interp.wav periodic_interp
```

Source modes:

- `sine`: steady 440 Hz tone.
- `chirp`: 220 Hz to 880 Hz sweep.

```sh
./build/debug/udp_audio_loopback 100 20 25 1337 1 recordings/chirp_periodic_interp.wav periodic_interp chirp
```

## Run The Opus Baseline

If `libopus` is installed, CMake builds an Opus loopback target:

```sh
./build/debug/udp_audio_opus_loopback [frames] [loss_percent] [jitter_ms] [seed] [record_wav] [source_mode] [bitrate_bps]
```

Example:

```sh
./build/debug/udp_audio_opus_loopback 100 20 25 1337 recordings/chirp_opus_plc.wav chirp
```

Missing packets are concealed by the Opus decoder and counted as `opus_plc_frames`
in the summary.

## Run The Playback Demo

```sh
./build/debug/udp_audio_miniaudio_playback 3
```

This plays a short 440 Hz tone through the default output device.
