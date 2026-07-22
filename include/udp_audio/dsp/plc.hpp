#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace udp_audio::dsp {

template <std::size_t FrameSamples>
class HoldAndDecayPlc {
 public:
  static constexpr float kGainDecayPerLostFrame = 0.70794576F;

  void accept_good_frame(std::span<const float, FrameSamples> frame) noexcept {
    for (std::size_t i = 0; i < FrameSamples; ++i) {
      last_good_frame_[i] = frame[i];
    }
    concealment_gain_ = 1.0F;
  }

  void synthesize_missing_frame(std::span<float, FrameSamples> out) noexcept {
    for (std::size_t i = 0; i < FrameSamples; ++i) {
      out[i] = last_good_frame_[i] * concealment_gain_;
    }
    concealment_gain_ *= kGainDecayPerLostFrame;
  }

 private:
  std::array<float, FrameSamples> last_good_frame_{};
  float concealment_gain_ = 1.0F;
};

}  // namespace udp_audio::dsp

