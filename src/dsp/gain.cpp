#include "udp_audio/dsp/gain.hpp"

namespace udp_audio::dsp {

#if defined(UDP_AUDIO_HAS_AVX2)
void apply_gain_avx2(std::span<float> samples, float gain) noexcept;
float peak_abs_avx2(std::span<const float> samples) noexcept;
#endif

void apply_gain(std::span<float> samples, float gain) noexcept {
#if defined(UDP_AUDIO_HAS_AVX2)
  apply_gain_avx2(samples, gain);
#else
  apply_gain_scalar(samples, gain);
#endif
}

float peak_abs(std::span<const float> samples) noexcept {
#if defined(UDP_AUDIO_HAS_AVX2)
  return peak_abs_avx2(samples);
#else
  return peak_abs_scalar(samples);
#endif
}

}  // namespace udp_audio::dsp

