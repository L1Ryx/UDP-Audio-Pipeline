#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace udp_audio::jitter {

struct FixedJitterBufferStats {
  std::size_t target_depth_frames = 0;
  std::uint64_t inserted = 0;
  std::uint64_t popped = 0;
  std::uint64_t underruns = 0;
  std::uint64_t late_or_collided_packets = 0;
};

template <typename Frame, std::size_t Capacity>
class FixedJitterBuffer {
 public:
  static_assert(Capacity >= 4);

  explicit FixedJitterBuffer(std::size_t target_depth_frames) noexcept {
    stats_.target_depth_frames = target_depth_frames;
  }

  bool push(std::uint32_t sequence, const Frame& frame) noexcept {
    const auto slot = sequence % Capacity;
    if (occupied_[slot] && sequences_[slot] != sequence) {
      ++stats_.late_or_collided_packets;
      return false;
    }

    frames_[slot] = frame;
    sequences_[slot] = sequence;
    occupied_[slot] = true;
    ++stats_.inserted;
    return true;
  }

  std::optional<Frame> pop_expected(std::uint32_t sequence) noexcept {
    const auto slot = sequence % Capacity;
    if (!occupied_[slot] || sequences_[slot] != sequence) {
      ++stats_.underruns;
      return std::nullopt;
    }

    occupied_[slot] = false;
    ++stats_.popped;
    return frames_[slot];
  }

  std::optional<Frame> peek_expected(std::uint32_t sequence) const noexcept {
    const auto slot = sequence % Capacity;
    if (!occupied_[slot] || sequences_[slot] != sequence) {
      return std::nullopt;
    }

    return frames_[slot];
  }

  [[nodiscard]] const FixedJitterBufferStats& stats() const noexcept {
    return stats_;
  }

 private:
  std::array<Frame, Capacity> frames_{};
  std::array<std::uint32_t, Capacity> sequences_{};
  std::array<bool, Capacity> occupied_{};
  FixedJitterBufferStats stats_{};
};

}  // namespace udp_audio::jitter
