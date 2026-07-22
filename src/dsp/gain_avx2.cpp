#include "udp_audio/dsp/gain.hpp"

#include <algorithm>
#include <immintrin.h>

namespace udp_audio::dsp {

void apply_gain_avx2(std::span<float> samples, float gain) noexcept {
  const auto gain_vec = _mm256_set1_ps(gain);
  std::size_t i = 0;

  for (; i + 8U <= samples.size(); i += 8U) {
    auto values = _mm256_loadu_ps(samples.data() + i);
    values = _mm256_mul_ps(values, gain_vec);
    _mm256_storeu_ps(samples.data() + i, values);
  }

  for (; i < samples.size(); ++i) {
    samples[i] *= gain;
  }
}

float peak_abs_avx2(std::span<const float> samples) noexcept {
  const auto sign_mask = _mm256_set1_ps(-0.0F);
  auto max_values = _mm256_setzero_ps();
  std::size_t i = 0;

  for (; i + 8U <= samples.size(); i += 8U) {
    const auto values = _mm256_loadu_ps(samples.data() + i);
    const auto abs_values = _mm256_andnot_ps(sign_mask, values);
    max_values = _mm256_max_ps(max_values, abs_values);
  }

  alignas(32) float lanes[8]{};
  _mm256_store_ps(lanes, max_values);

  float peak = 0.0F;
  for (const float lane : lanes) {
    peak = std::max(peak, lane);
  }

  for (; i < samples.size(); ++i) {
    peak = std::max(peak, samples[i] < 0.0F ? -samples[i] : samples[i]);
  }

  return peak;
}

}  // namespace udp_audio::dsp

