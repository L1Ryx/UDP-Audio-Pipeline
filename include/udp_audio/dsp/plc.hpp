#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace udp_audio::dsp {

enum class PlcSynthesisMode {
  repeat_last_frame,
  periodic,
  periodic_interpolated,
};

template <std::size_t FrameSamples>
class HoldAndDecayPlc {
 public:
  static constexpr float kGainDecayPerLostFrame = 0.70794576F;

  void accept_good_frame(std::span<const float, FrameSamples> frame) noexcept {
    for (std::size_t i = 0; i < FrameSamples; ++i) {
      last_good_frame_[i] = frame[i];
      append_history(frame[i]);
    }
    concealment_gain_ = 1.0F;
  }

  void synthesize_missing_frame(
    std::span<float, FrameSamples> out,
    PlcSynthesisMode mode = PlcSynthesisMode::periodic_interpolated) noexcept {
    const auto period =
      mode == PlcSynthesisMode::periodic || mode == PlcSynthesisMode::periodic_interpolated
        ? estimate_period()
        : PeriodEstimate{};

    for (std::size_t i = 0; i < FrameSamples; ++i) {
      const float source =
        period.valid
          ? (mode == PlcSynthesisMode::periodic_interpolated
               ? sample_from_recent(period.fractional_lag)
               : sample_from_recent(period.integer_lag))
          : last_good_frame_[i];
      out[i] = source * concealment_gain_;
      append_history(out[i]);
    }

    concealment_gain_ *= kGainDecayPerLostFrame;
  }

 private:
  static constexpr std::size_t kHistorySamples = FrameSamples * 4U;

  struct PeriodEstimate {
    std::size_t integer_lag = 0;
    float fractional_lag = 0.0F;
    bool valid = false;
  };

  void append_history(float sample) noexcept {
    history_[history_write_index_] = sample;
    history_write_index_ = (history_write_index_ + 1U) % kHistorySamples;
    history_size_ = std::min(history_size_ + 1U, kHistorySamples);
  }

  [[nodiscard]] float sample_from_recent(std::size_t offset_from_end) const noexcept {
    if (history_size_ == 0) {
      return 0.0F;
    }

    const auto clamped_offset = std::min(offset_from_end, history_size_);
    const auto index = (history_write_index_ + kHistorySamples - clamped_offset) % kHistorySamples;
    return history_[index];
  }

  [[nodiscard]] float sample_from_recent(float offset_from_end) const noexcept {
    if (history_size_ == 0) {
      return 0.0F;
    }

    const float clamped_offset =
      std::clamp(offset_from_end, 1.0F, static_cast<float>(history_size_));
    const auto newer_offset = static_cast<std::size_t>(std::floor(clamped_offset));
    const auto older_offset = std::min(newer_offset + 1U, history_size_);
    const float blend = clamped_offset - static_cast<float>(newer_offset);

    const float newer = sample_from_recent(newer_offset);
    const float older = sample_from_recent(older_offset);
    return newer + (older - newer) * blend;
  }

  [[nodiscard]] float correlation_score(std::size_t period, std::size_t window) const noexcept {
    float cross = 0.0F;
    float recent_energy = 0.0F;
    float delayed_energy = 0.0F;

    for (std::size_t i = 0; i < window; ++i) {
      const auto recent_offset = window - i;
      const auto delayed_offset = recent_offset + period;
      const float recent = sample_from_recent(recent_offset);
      const float delayed = sample_from_recent(delayed_offset);

      cross += recent * delayed;
      recent_energy += recent * recent;
      delayed_energy += delayed * delayed;
    }

    const float denominator = std::sqrt(recent_energy * delayed_energy);
    return denominator > 0.000001F ? cross / denominator : 0.0F;
  }

  [[nodiscard]] PeriodEstimate estimate_period() const noexcept {
    if (history_size_ < 16U) {
      return {};
    }

    const auto min_period = std::max<std::size_t>(1U, FrameSamples / 20U);
    const auto max_period = std::min<std::size_t>(FrameSamples, history_size_ / 2U);
    if (max_period <= min_period) {
      return {};
    }

    const auto window = std::min<std::size_t>(FrameSamples, history_size_ - max_period);
    if (window < 8U) {
      return {};
    }

    std::size_t best_period = 0;
    float best_score = 0.0F;

    for (std::size_t period = min_period; period <= max_period; ++period) {
      const float score = correlation_score(period, window);
      if (score > best_score) {
        best_score = score;
        best_period = period;
      }
    }

    if (best_score < 0.55F) {
      return {};
    }

    float fractional_period = static_cast<float>(best_period);
    if (best_period > min_period && best_period < max_period) {
      const float left = correlation_score(best_period - 1U, window);
      const float right = correlation_score(best_period + 1U, window);
      const float denominator = left - (2.0F * best_score) + right;
      if (std::fabs(denominator) > 0.000001F) {
        const float offset = 0.5F * (left - right) / denominator;
        fractional_period += std::clamp(offset, -0.5F, 0.5F);
      }
    }

    return PeriodEstimate{best_period, fractional_period, true};
  }

  std::array<float, FrameSamples> last_good_frame_{};
  std::array<float, kHistorySamples> history_{};
  std::size_t history_write_index_ = 0;
  std::size_t history_size_ = 0;
  float concealment_gain_ = 1.0F;
};

}  // namespace udp_audio::dsp
