#include "udp_audio/dsp/gain.hpp"

#include <algorithm>
#include <arm_neon.h>

namespace udp_audio::dsp {

void apply_gain_neon(std::span<float> samples, float gain) noexcept {
  const auto gain_vec = vdupq_n_f32(gain);
  std::size_t i = 0;

  for (; i + 16U <= samples.size(); i += 16U) {
    auto values0 = vld1q_f32(samples.data() + i);
    auto values1 = vld1q_f32(samples.data() + i + 4U);
    auto values2 = vld1q_f32(samples.data() + i + 8U);
    auto values3 = vld1q_f32(samples.data() + i + 12U);
    values0 = vmulq_f32(values0, gain_vec);
    values1 = vmulq_f32(values1, gain_vec);
    values2 = vmulq_f32(values2, gain_vec);
    values3 = vmulq_f32(values3, gain_vec);
    vst1q_f32(samples.data() + i, values0);
    vst1q_f32(samples.data() + i + 4U, values1);
    vst1q_f32(samples.data() + i + 8U, values2);
    vst1q_f32(samples.data() + i + 12U, values3);
  }

  for (; i + 4U <= samples.size(); i += 4U) {
    auto values = vld1q_f32(samples.data() + i);
    values = vmulq_f32(values, gain_vec);
    vst1q_f32(samples.data() + i, values);
  }

  for (; i < samples.size(); ++i) {
    samples[i] *= gain;
  }
}

float peak_abs_neon(std::span<const float> samples) noexcept {
  auto max_values0 = vdupq_n_f32(0.0F);
  auto max_values1 = vdupq_n_f32(0.0F);
  auto max_values2 = vdupq_n_f32(0.0F);
  auto max_values3 = vdupq_n_f32(0.0F);
  std::size_t i = 0;

  for (; i + 16U <= samples.size(); i += 16U) {
    max_values0 = vmaxq_f32(max_values0, vabsq_f32(vld1q_f32(samples.data() + i)));
    max_values1 = vmaxq_f32(max_values1, vabsq_f32(vld1q_f32(samples.data() + i + 4U)));
    max_values2 = vmaxq_f32(max_values2, vabsq_f32(vld1q_f32(samples.data() + i + 8U)));
    max_values3 = vmaxq_f32(max_values3, vabsq_f32(vld1q_f32(samples.data() + i + 12U)));
  }

  auto max_values = vmaxq_f32(vmaxq_f32(max_values0, max_values1),
                              vmaxq_f32(max_values2, max_values3));

  for (; i + 4U <= samples.size(); i += 4U) {
    const auto values = vld1q_f32(samples.data() + i);
    max_values = vmaxq_f32(max_values, vabsq_f32(values));
  }

#if defined(__aarch64__)
  float peak = vmaxvq_f32(max_values);
#else
  alignas(16) float lanes[4]{};
  vst1q_f32(lanes, max_values);
  float peak = 0.0F;
  for (const float lane : lanes) {
    peak = std::max(peak, lane);
  }
#endif

  for (; i < samples.size(); ++i) {
    peak = std::max(peak, samples[i] < 0.0F ? -samples[i] : samples[i]);
  }

  return peak;
}

}  // namespace udp_audio::dsp
