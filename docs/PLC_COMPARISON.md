# PLC Comparison Notes

These measurements compare the core packet-loss concealment modes using the same
deterministic loopback scenario:

```sh
./cmake-build-debug/udp_audio_loopback 50 20 25 1337 0 recordings/plc_none.wav none sine
./cmake-build-debug/udp_audio_loopback 50 20 25 1337 0 recordings/plc_repeat.wav repeat sine
./cmake-build-debug/udp_audio_loopback 50 20 25 1337 0 recordings/plc_periodic.wav periodic sine
./cmake-build-debug/udp_audio_loopback 50 20 25 1337 0 recordings/plc_periodic_interp.wav periodic_interp sine
```

Baseline:

```sh
./cmake-build-debug/udp_audio_loopback 50 0 0 1337 0 recordings/clean_50.wav periodic_interp sine
```

## Sine Test

- 440 Hz sine wave
- 48 kHz mono float PCM
- 10 ms engine frames
- 50 total frames
- 20% deterministic packet loss
- Up to 25 ms deterministic delivery jitter
- Seed: 1337

| Mode | Peak | RMS Error vs Clean | Max Error vs Clean | Max Adjacent Delta |
|---|---:|---:|---:|---:|
| none | 0.273106 | 0.051334 | 0.296620 | 0.013519 |
| repeat | 0.200000 | 0.000866 | 0.011513 | 0.012688 |
| periodic | 0.200000 | 0.000866 | 0.011513 | 0.012688 |
| periodic_interp | 0.200000 | 0.000021 | 0.000152 | 0.011565 |

## Chirp Test

The loopback demo also supports a changing-pitch chirp source:

```sh
./cmake-build-debug/udp_audio_loopback 50 0 0 1337 0 recordings/chirp_clean_50.wav periodic_interp chirp
./cmake-build-debug/udp_audio_loopback 50 20 25 1337 0 recordings/chirp_none.wav none chirp
./cmake-build-debug/udp_audio_loopback 50 20 25 1337 0 recordings/chirp_repeat.wav repeat chirp
./cmake-build-debug/udp_audio_loopback 50 20 25 1337 0 recordings/chirp_periodic.wav periodic chirp
./cmake-build-debug/udp_audio_loopback 50 20 25 1337 0 recordings/chirp_periodic_interp.wav periodic_interp chirp
```

This sweeps from 220 Hz to 880 Hz over the 50-frame run. It is harder than the steady
sine wave because the expected period changes during a lost packet.

| Mode | Peak | RMS Error vs Clean | Max Error vs Clean | Max Adjacent Delta |
|---|---:|---:|---:|---:|
| none | 0.343722 | 0.051531 | 0.322695 | 0.024803 |
| repeat | 0.200481 | 0.027515 | 0.219705 | 0.207152 |
| periodic | 0.200481 | 0.027515 | 0.219705 | 0.207152 |
| periodic_interp | 0.200000 | 0.024291 | 0.194438 | 0.201478 |

## Interpretation

- `none` exposes the missing-frame problem directly: the output drops toward silence.
- `repeat` is now pitch/correlation-based repetition when a reliable recent period is
  available. It no longer blindly replays the last 10 ms packet.
- `periodic` currently matches `repeat`: it repeats from one whole-sample estimated
  period ago.
- `periodic_interp` refines the estimated period with fractional-sample interpolation.
  This is the best current mode on both test signals.

The original naive repeat popped because it repeated a 10 ms packet. For a 440 Hz sine,
10 ms contains 4.4 cycles, so repeating the whole packet restarts the waveform at the
wrong phase. The fixed repeat path instead repeats from the estimated waveform period.

The chirp still exposes a real limitation: pitch changes during the missing frame, so a
period estimated before the loss can be stale when real audio resumes. That remaining
artifact should be addressed by improving the core PLC/resynchronization strategy, not
by adding more comparison modes.

## Opus PLC Baseline

Opus gives us a serious reference implementation for packet-loss concealment. In this
target, the sender encodes every 10 ms float PCM frame into an Opus packet, the same UDP
impairment layer drops or delays encoded packets, and the receiver decodes in playout
order. When a frame is missing, the receiver calls the Opus decoder with a null packet
for exactly one 480-sample frame.

```sh
./cmake-build-debug/udp_audio_opus_loopback 50 0 0 1337 recordings/opus_sine_clean_50.wav sine
./cmake-build-debug/udp_audio_opus_loopback 50 20 25 1337 recordings/opus_sine_plc.wav sine
./cmake-build-debug/udp_audio_opus_loopback 50 0 0 1337 recordings/chirp_opus_clean_50.wav chirp
./cmake-build-debug/udp_audio_opus_loopback 50 20 25 1337 recordings/chirp_opus_plc.wav chirp
./cmake-build-debug/udp_audio_opus_loopback 50 20 25 1337 recordings/chirp_opus_fec.wav chirp 64000 fec
```

These metrics compare the impaired Opus output against a clean Opus encode/decode
reference, so codec coloration is not counted as PLC error.

| Source | Peak | RMS Error vs Clean Opus | Max Error vs Clean Opus | Max Adjacent Delta |
|---|---:|---:|---:|---:|
| sine | 0.204352 | 0.009511 | 0.099150 | 0.012298 |
| chirp | 0.229507 | 0.078361 | 0.387838 | 0.019942 |

The chirp still has meaningful waveform error because no PLC can know the exact missing
frequency trajectory. The important improvement is continuity: the max adjacent delta
stays near normal waveform motion instead of jumping to the roughly `0.20` discontinuity
seen in the homegrown chirp modes.

### Opus FEC Check

Opus in-band FEC adds repair data to later packets. The receiver can recover a missing
frame from the following packet if that following packet arrives before playout. With
the same chirp impairment pattern, 6 packets were missing, 5 frames were recovered using
FEC, and 1 frame fell back to normal Opus PLC.

| Strategy | FEC Frames | PLC Frames | RMS Error vs Clean Opus | Max Error vs Clean Opus | Max Adjacent Delta |
|---|---:|---:|---:|---:|---:|
| Opus PLC | 0 | 6 | 0.078361 | 0.387838 | 0.019942 |
| Opus FEC | 5 | 1 | 0.033517 | 0.136705 | 0.022919 |

This is the first real recovery step beyond concealment. It does not make packet loss
free, and it needs the next packet to arrive in time, but it preserves much more of the
actual chirp than decoder PLC alone.
