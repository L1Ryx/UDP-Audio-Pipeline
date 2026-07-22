#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace udp_audio::jitter {

struct JitterBufferStats {
  double mean_inter_arrival_ms = 0.0;
  double variance_inter_arrival_ms = 0.0;
  std::size_t target_depth_frames = 3;
  std::uint64_t underruns = 0;
  std::uint64_t late_packets = 0;
};

template <typename Frame, std::size_t Capacity>
class AdaptiveJitterBuffer {
 public:
  static_assert(Capacity >= 4);

  explicit AdaptiveJitterBuffer(std::size_t min_depth_frames = 2,
                                std::size_t max_depth_frames = Capacity / 2U)
      : min_depth_frames_(min_depth_frames),
        max_depth_frames_(std::max(min_depth_frames, max_depth_frames)) {
    stats_.target_depth_frames = min_depth_frames_;
  }

  void observe_inter_arrival(double inter_arrival_ms) noexcept {
    constexpr double kAlpha = 0.05;
    const double delta = inter_arrival_ms - stats_.mean_inter_arrival_ms;
    stats_.mean_inter_arrival_ms += kAlpha * delta;
    stats_.variance_inter_arrival_ms =
      (1.0 - kAlpha) * (stats_.variance_inter_arrival_ms + kAlpha * delta * delta);

    const auto suggested = static_cast<std::size_t>(
      std::clamp(2.0 + stats_.variance_inter_arrival_ms, static_cast<double>(min_depth_frames_),
                 static_cast<double>(max_depth_frames_)));
    stats_.target_depth_frames = suggested;
  }

  bool push(std::uint32_t sequence, const Frame& frame) noexcept {
    const auto slot = sequence % Capacity;
    if (occupied_[slot] && sequences_[slot] != sequence) {
      ++stats_.late_packets;
      return false;
    }

    frames_[slot] = frame;
    sequences_[slot] = sequence;
    occupied_[slot] = true;
    return true;
  }

  std::optional<Frame> pop_expected(std::uint32_t sequence) noexcept {
    const auto slot = sequence % Capacity;
    if (!occupied_[slot] || sequences_[slot] != sequence) {
      ++stats_.underruns;
      return std::nullopt;
    }

    occupied_[slot] = false;
    return frames_[slot];
  }

  [[nodiscard]] const JitterBufferStats& stats() const noexcept {
    return stats_;
  }

 private:
  std::array<Frame, Capacity> frames_{};
  std::array<std::uint32_t, Capacity> sequences_{};
  std::array<bool, Capacity> occupied_{};
  std::size_t min_depth_frames_ = 2;
  std::size_t max_depth_frames_ = Capacity / 2U;
  JitterBufferStats stats_{};
};

}  // namespace udp_audio::jitter

