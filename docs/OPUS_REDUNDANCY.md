# Opus Redundancy Notes

The Opus loopback now carries redundant repair packets inside each UDP payload. The
primary Opus packet is still decoded normally. Each packet can also include up to three
previous Opus packets, so if a primary packet is dropped, the receiver can often decode
the real encoded audio from a later surviving packet.

```sh
./cmake-build-debug/udp_audio_opus_loopback 100 20 25 1337 recordings/chirp_opus_fixed_burst.wav chirp 64000 fec 0
```

## Burst-Loss Repro

The earlier 100-frame chirp run produced a visible and audible dropout around frames
59-61. The cause was a burst of dropped primary packets. Opus PLC avoided clicks, but
after repeated missing frames it faded almost to silence.

| Run | Jitter Depth | Redundant Frames | Missing Frames | Redundancy Recovered | PLC Frames |
|---|---:|---:|---:|---:|---:|
| Opus FEC only | 3 | 0 | 23 | 0 | 9 |
| Redundancy, tight depth | 3 | 3 | 23 | 17 | 6 |
| Redundancy, playback depth | 5 | 3 | 23 | 22 | 1 |

Frame RMS around the audible dropout:

| Frame | FEC Only | Redundancy Depth 3 | Redundancy Depth 5 |
|---|---:|---:|---:|
| 59 | 0.008117 | 0.082725 | 0.141828 |
| 60 | 0.000243 | 0.015403 | 0.140226 |
| 61 | 0.000304 | 0.073749 | 0.140344 |

The fix is not better concealment. It is avoiding concealment by recovering real Opus
packets when LAN bandwidth and playback latency allow it.
