# Impairment Presets

Lossy Audio Lab includes repeatable synthetic network presets for previewing how
audio behaves under packet loss and jitter.

The presets are not exact models of any specific router, carrier, or conferencing
stack. They are controlled impairment scenarios for comparing recovery behavior with
the same seed, audio source, jitter buffer, Opus redundancy, and PLC path.

## Loss Models

- `independent`: each frame has its own drop chance.
- `burst`: loss starts probabilistically, then drops a short run of consecutive
  frames. This better approximates bad wireless moments where several packets vanish
  together.

## Current Presets

| Preset | Loss Model | Loss % | Jitter ms | Redundant Frames | Jitter Depth |
|---|---|---:|---:|---:|---:|
| Clean LAN | independent | 0 | 1 | 0 | 3 |
| Same-Room Wi-Fi | independent | 3 | 8 | 1 | 4 |
| Busy Wi-Fi | burst | 10 | 20 | 2 | 5 |
| Weak Wi-Fi | burst | 14 | 30 | 3 | 7 |
| Bad Hotspot | burst | 20 | 45 | 3 | 8 |
| Stress | burst | 35 | 65 | 3 | 10 |

The useful comparison is not whether a preset perfectly matches a real network. The
useful comparison is how the recovery timeline, RMS dips, audible output, and latency
change when the impairment pattern becomes harsher.

## Seeds

Runs are deterministic when the seed stays the same. This is intentional: repeatable
loss schedules make A/B listening and screenshots easier. Use `New Seed` for a fresh
scenario, or enable `Reroll Seed On Run` when exploring many possible loss patterns.

## User Audio

The ImGui lab can load WAV or MP3 files through miniaudio. Imported audio is decoded
to the project-native format: 48 kHz, mono, 32-bit float PCM. The simulator then cuts
the decoded stream into the same 10 ms / 480-sample frames used by the generated sine
and chirp sources.

That means user audio goes through the real comparison path:

1. Decode WAV/MP3 to mono 48 kHz float PCM.
2. Packetize into 10 ms frames.
3. Encode each frame with Opus.
4. Apply independent or burst packet loss plus jitter.
5. Recover with redundancy/FEC/PLC according to the selected recovery mode.
6. Display the waveform, frame RMS, recovery timeline, and summary metrics.

The UI currently imports up to the first 5 minutes of a file. This keeps full-song
previews useful while still avoiding unbounded memory use. Presets only change
network/recovery settings, so a loaded file remains active while comparing Clean LAN,
Wi-Fi, hotspot, and stress cases.

For a quick decoder/simulator smoke test without opening the UI:

```sh
./cmake-build-debug/lossy_audio_lab --headless-file /path/to/audio.wav
./cmake-build-debug/lossy_audio_lab --headless-file /path/to/audio.mp3
```
