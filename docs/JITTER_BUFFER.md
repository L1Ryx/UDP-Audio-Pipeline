# Jitter Buffer Notes

The loopback demo can now compare fixed and adaptive receiver playout depth.

```sh
./cmake-build-debug/udp_audio_loopback 80 0 45 1337 0 recordings/jitter_fixed_45ms.wav periodic_interp chirp fixed
./cmake-build-debug/udp_audio_loopback 80 0 45 1337 0 recordings/jitter_adaptive_45ms.wav periodic_interp chirp adaptive
```

This test uses no packet loss and up to 45 ms of delivery jitter. That isolates packets
that are missing because they arrive too late for playout, not because they were dropped.

| Mode | Concealed Frames | Late Datagrams | Avg Depth | Max Depth | Avg Playout Latency |
|---|---:|---:|---:|---:|---:|
| fixed | 18 | 18 | 3.0 | 3 | 32.0236 ms |
| adaptive | 1 | 1 | 4.8 | 6 | 51.5833 ms |

## Interpretation

The fixed buffer plays every frame about 30 ms behind send time. That is not enough for
this stress case, so 18 packets arrive after their playout slot and have to be concealed.

The adaptive buffer starts at the same 3-frame depth, then watches inter-arrival timing.
When packet spacing becomes bursty, it raises the target depth. In this run it averaged
4.8 frames, peaked at 6 frames, and reduced timing-caused concealment from 18 frames to
1 frame.

This is the central jitter-buffer tradeoff: fewer underruns in exchange for more playout
latency. For the later ImGui view, the useful live plots will be target depth, late
datagrams, underruns, inter-arrival time, and playout latency.
