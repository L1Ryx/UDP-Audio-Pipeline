# Project Brief

## Goal

Build a portfolio-grade C++20 UDP audio pipeline that proves low-latency systems work,
audio robustness under packet loss, and explicit AVX2 optimization.

## Reduced Scope

The project targets LAN and loopback measurements, not Internet-scale transport. The
primary performance environment is x86/x64 Linux with AVX2. The codebase may build on
other machines for development convenience, but cross-platform support is not a core
deliverable.

## Must-Have Features

- Custom UDP packet protocol with 16-byte binary header.
- Lock-free SPSC queue between network and audio-processing stages.
- Receiver-side jitter buffer with adaptive target depth.
- Packet loss concealment that avoids silence/clicks during missing frames.
- AVX2 DSP kernels for at least gain and peak detection, then limiter/downmix.
- Repeatable LAN/loopback benchmarks for latency, jitter, underruns, and DSP throughput.

## Nice-To-Have Later

- Pitch-period or WSOLA-based PLC after the hold-and-decay concealer is benchmarked.
- A network impairment proxy for deterministic packet loss and jitter.
- ImGui or Python plots for telemetry visualization.
- Real mic/speaker I/O through miniaudio once the headless pipeline is stable.

## Portfolio Proof

- Architecture diagram.
- Short code excerpt explaining SPSC acquire/release ordering.
- SIMD benchmark chart: scalar vs AVX2.
- Packet-loss demo audio: no concealment vs PLC enabled.
- LAN latency and jitter-buffer telemetry table.

## Milestones

1. Compileable core skeleton with tests.
2. UDP sender/receiver loopback demo.
3. Fixed jitter buffer with underrun tracking.
4. Adaptive jitter target and telemetry output.
5. PLC quality upgrade and A/B audio export.
6. AVX2 benchmark executable.
7. Final demo assets and portfolio writeup.

