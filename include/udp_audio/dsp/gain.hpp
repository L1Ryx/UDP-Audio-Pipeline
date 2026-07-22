#pragma once

#include <cstddef>
#include <span>

namespace udp_audio::dsp {

void apply_gain_scalar(std::span<float> samples, float gain) noexcept;
void apply_gain(std::span<float> samples, float gain) noexcept;

float peak_abs_scalar(std::span<const float> samples) noexcept;
float peak_abs(std::span<const float> samples) noexcept;

}  // namespace udp_audio::dsp

