#include "udp_audio/audio/frame.hpp"

namespace udp_audio::audio {

MonoAudioFrame make_silent_frame(std::uint32_t sequence) noexcept {
  MonoAudioFrame frame{};
  frame.sequence = sequence;
  frame.timestamp_samples = sequence * static_cast<std::uint32_t>(kFrameSamples);
  return frame;
}

}  // namespace udp_audio::audio

