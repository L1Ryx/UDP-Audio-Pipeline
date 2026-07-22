#pragma once

#include "udp_audio/audio/frame.hpp"

#include <cstdint>
#include <cstddef>

namespace udp_audio::audio {

enum class SourceMode {
  sine,
  chirp,
};

struct SourceState {
  double phase = 0.0;
};

MonoAudioFrame make_source_frame(SourceMode source_mode,
                                 std::uint32_t sequence,
                                 std::size_t total_frames,
                                 SourceState& state) noexcept;

}  // namespace udp_audio::audio
