#pragma once

#include <cstddef>
#include <span>

namespace udp_audio::dsp {

void apply_gain_scalar(std::span<float> samples, float gain) noexcept;
void apply_gain(std::span<float> samples, float gain) noexcept;

float peak_abs_scalar(std::span<const float> samples) noexcept;
float peak_abs(std::span<const float> samples) noexcept;

bool gain_dispatch_uses_avx2() noexcept;
bool gain_dispatch_uses_neon() noexcept;
const char* gain_dispatch_backend() noexcept;

}  // namespace udp_audio::dsp
