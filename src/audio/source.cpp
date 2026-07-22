#include "udp_audio/audio/source.hpp"

#include <algorithm>
#include <cmath>

namespace udp_audio::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 440.0;
constexpr double kChirpStartHz = 220.0;
constexpr double kChirpEndHz = 880.0;
constexpr float kToneGain = 0.2F;

MonoAudioFrame make_sine_frame(std::uint32_t sequence, SourceState& state) noexcept {
  MonoAudioFrame frame{};
  frame.sequence = sequence;
  frame.timestamp_samples = sequence * static_cast<std::uint32_t>(kFrameSamples);

  const double phase_step = (2.0 * kPi * kToneHz) / static_cast<double>(kSampleRateHz);
  for (float& sample : frame.samples) {
    sample = static_cast<float>(std::sin(state.phase)) * kToneGain;
    state.phase += phase_step;
    if (state.phase >= 2.0 * kPi) {
      state.phase -= 2.0 * kPi;
    }
  }

  return frame;
}

MonoAudioFrame make_chirp_frame(std::uint32_t sequence,
                                std::size_t total_frames,
                                SourceState& state) noexcept {
  MonoAudioFrame frame{};
  frame.sequence = sequence;
  frame.timestamp_samples = sequence * static_cast<std::uint32_t>(kFrameSamples);

  const auto total_samples = std::max<std::size_t>(1U, total_frames * kFrameSamples);
  for (std::size_t i = 0; i < frame.samples.size(); ++i) {
    const auto absolute_sample = static_cast<std::size_t>(sequence) * kFrameSamples + i;
    const double progress =
      static_cast<double>(std::min(absolute_sample, total_samples - 1U)) /
      static_cast<double>(total_samples - 1U);
    const double frequency = kChirpStartHz + ((kChirpEndHz - kChirpStartHz) * progress);
    const double phase_step = (2.0 * kPi * frequency) / static_cast<double>(kSampleRateHz);

    frame.samples[i] = static_cast<float>(std::sin(state.phase)) * kToneGain;
    state.phase += phase_step;
    if (state.phase >= 2.0 * kPi) {
      state.phase -= 2.0 * kPi;
    }
  }

  return frame;
}

}  // namespace

MonoAudioFrame make_source_frame(SourceMode source_mode,
                                 std::uint32_t sequence,
                                 std::size_t total_frames,
                                 SourceState& state) noexcept {
  if (source_mode == SourceMode::chirp) {
    return make_chirp_frame(sequence, total_frames, state);
  }
  return make_sine_frame(sequence, state);
}

}  // namespace udp_audio::audio
