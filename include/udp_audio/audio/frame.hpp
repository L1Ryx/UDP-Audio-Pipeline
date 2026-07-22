#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace udp_audio::audio {

inline constexpr std::uint32_t kSampleRateHz = 48'000;
inline constexpr std::uint32_t kFrameDurationMs = 10;
inline constexpr std::size_t kChannels = 1;
inline constexpr std::size_t kSamplesPerChannel =
  static_cast<std::size_t>(kSampleRateHz / (1'000 / kFrameDurationMs));
inline constexpr std::size_t kFrameSamples = kSamplesPerChannel * kChannels;
inline constexpr std::size_t kFramePayloadBytes = kFrameSamples * sizeof(float);

struct MonoAudioFrame {
  std::uint32_t sequence = 0;
  std::uint32_t timestamp_samples = 0;
  alignas(32) std::array<float, kFrameSamples> samples{};

  [[nodiscard]] std::span<float> sample_span() noexcept {
    return samples;
  }

  [[nodiscard]] std::span<const float> sample_span() const noexcept {
    return samples;
  }
};

MonoAudioFrame make_silent_frame(std::uint32_t sequence) noexcept;

}  // namespace udp_audio::audio

