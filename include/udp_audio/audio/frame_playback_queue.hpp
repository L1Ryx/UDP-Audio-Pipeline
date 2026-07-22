#pragma once

#include "udp_audio/audio/frame.hpp"
#include "udp_audio/concurrency/spsc_ring_buffer.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace udp_audio::audio {

struct FramePlaybackStats {
  std::atomic<std::uint64_t> preroll_frames{0};
  std::atomic<std::uint64_t> fadeout_frames{0};
  std::atomic<std::uint64_t> enqueued_output_frames{0};
  std::atomic<std::uint64_t> rendered_device_frames{0};
  std::atomic<std::uint64_t> callback_underruns{0};
  std::atomic<std::uint64_t> dropped_output_frames{0};
};

template <std::size_t CapacityFrames>
class FramePlaybackQueue {
 public:
  void enqueue_preroll(std::size_t frame_count) {
    const auto silent_frame = make_silent_frame(0);
    for (std::size_t i = 0; i < frame_count; ++i) {
      if (queue_.push(silent_frame)) {
        stats_.preroll_frames.fetch_add(1, std::memory_order_relaxed);
      } else {
        stats_.dropped_output_frames.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void enqueue_output(const MonoAudioFrame& frame) {
    if (queue_.push(frame)) {
      stats_.enqueued_output_frames.fetch_add(1, std::memory_order_relaxed);
    } else {
      stats_.dropped_output_frames.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void enqueue_fadeout(const MonoAudioFrame& frame) {
    if (queue_.push(frame)) {
      stats_.fadeout_frames.fetch_add(1, std::memory_order_relaxed);
    } else {
      stats_.dropped_output_frames.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void render(float* output, std::size_t sample_count) {
    std::size_t written = 0;

    while (written < sample_count) {
      if (current_offset_ >= kFrameSamples) {
        auto next_frame = queue_.pop();
        if (!next_frame.has_value()) {
          const auto remaining = sample_count - written;
          std::fill(output + written, output + sample_count, 0.0F);
          stats_.callback_underruns.fetch_add(1, std::memory_order_relaxed);
          stats_.rendered_device_frames.fetch_add(remaining, std::memory_order_relaxed);
          return;
        }

        current_frame_ = *next_frame;
        current_offset_ = 0;
      }

      const auto available_in_frame = kFrameSamples - current_offset_;
      const auto remaining_output = sample_count - written;
      const auto samples_to_copy = std::min(available_in_frame, remaining_output);
      const auto* source = current_frame_.samples.data() + current_offset_;

      std::copy(source, source + samples_to_copy, output + written);

      written += samples_to_copy;
      current_offset_ += samples_to_copy;
      stats_.rendered_device_frames.fetch_add(samples_to_copy, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] FramePlaybackStats& stats() noexcept {
    return stats_;
  }

  [[nodiscard]] const FramePlaybackStats& stats() const noexcept {
    return stats_;
  }

 private:
  concurrency::SpscRingBuffer<MonoAudioFrame, CapacityFrames> queue_{};
  MonoAudioFrame current_frame_{};
  std::size_t current_offset_ = kFrameSamples;
  FramePlaybackStats stats_{};
};

}  // namespace udp_audio::audio
