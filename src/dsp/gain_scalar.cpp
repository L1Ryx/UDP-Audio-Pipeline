#include "udp_audio/dsp/gain.hpp"

#include <algorithm>
#include <cmath>

namespace udp_audio::dsp {

void apply_gain_scalar(std::span<float> samples, float gain) noexcept {
  for (float& sample : samples) {
    sample *= gain;
  }
}

float peak_abs_scalar(std::span<const float> samples) noexcept {
  float peak = 0.0F;
  for (const float sample : samples) {
    peak = std::max(peak, std::fabs(sample));
  }
  return peak;
}

}  // namespace udp_audio::dsp

