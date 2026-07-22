#include "udp_audio/audio/frame.hpp"

#include <cassert>

namespace {

void audio_frame_shape_is_10ms_float_pcm() {
  static_assert(udp_audio::audio::kSampleRateHz == 48'000);
  static_assert(udp_audio::audio::kFrameDurationMs == 10);
  static_assert(udp_audio::audio::kFrameSamples == 480);
  static_assert(udp_audio::audio::kFramePayloadBytes == 1'920);

  const auto frame = udp_audio::audio::make_silent_frame(3);
  assert(frame.sequence == 3);
  assert(frame.timestamp_samples == 1'440);
  assert(frame.samples.size() == udp_audio::audio::kFrameSamples);
}

}  // namespace

int test_audio_frame_main() {
  audio_frame_shape_is_10ms_float_pcm();
  return 0;
}

