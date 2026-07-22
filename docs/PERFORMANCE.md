# Performance Notes

Use `udp_audio_dsp_bench` to compare scalar DSP kernels with the active optimized
dispatch path.

```sh
./cmake-build-debug/udp_audio_dsp_bench
```

For meaningful optimization numbers, use a Release build:

```sh
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target udp_audio_dsp_bench
./cmake-build-release/udp_audio_dsp_bench
```

Optional arguments:

```sh
./cmake-build-release/udp_audio_dsp_bench [sample_count] [iterations]
```

For a longer, more stable run:

```sh
./cmake-build-release/udp_audio_dsp_bench 4800000 1000
```

The benchmark reports:

- `dispatch_backend`: `scalar`, `neon`, or `avx2`
- `avx2_available`: whether this build is using AVX2 dispatch
- `neon_available`: whether this build is using ARM NEON dispatch
- `apply_gain_speedup_vs_scalar`
- `peak_abs_speedup_vs_scalar`
- throughput in million samples per second

On Apple Silicon, `dispatch_backend=neon` is expected. AVX2 is an x86/x64
instruction set, so it is only active on Intel/AMD x86_64 builds with AVX2 enabled.

Apple Silicon Release baseline from this project machine:

```text
dispatch_backend=neon
avx2_available=0
neon_available=1
sample_count=4800000
iterations=1000
scalar_apply_gain_ms=351.912
dispatch_apply_gain_ms=335.825
apply_gain_speedup_vs_scalar=1.0479
scalar_peak_abs_ms=5653.39
dispatch_peak_abs_ms=230.618
peak_abs_speedup_vs_scalar=24.5141
```

The gain kernel is only a modest win because AppleClang can already auto-vectorize
simple scalar multiplication in Release builds. Peak detection benefits much more
from the explicit NEON horizontal max path.

## Writeup Takeaway

The main lesson is that SIMD optimization is workload-dependent. A simple gain pass
is easy for the compiler to optimize and is close to memory-bandwidth limited, so the
hand-written NEON path only measured about a 1.05x improvement in the stable Release
run. Peak detection is a reduction: scalar code repeatedly updates one running max,
while NEON can compute absolute values and maximums across multiple lanes before a
horizontal reduction. That made explicit NEON much more valuable for `peak_abs`,
where it measured about a 24.5x speedup.

For the project writeup, the honest performance story is not "SIMD makes everything
faster." It is that profiling identified which DSP primitives actually benefited
from explicit SIMD, and which ones the compiler already handled well.
