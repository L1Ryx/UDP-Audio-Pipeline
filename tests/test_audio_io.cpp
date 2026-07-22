#include "udp_audio/audio/frame_playback_queue.hpp"
#include "udp_audio/audio/wav_writer.hpp"

#include <array>
#include <cassert>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::uint32_t read_u32_le(const std::vector<unsigned char>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void wav_writer_patches_float_pcm_header() {
  const auto path = std::filesystem::temp_directory_path() / "udp_audio_wav_writer_test.wav";

  udp_audio::audio::WavWriter writer;
  assert(writer.open(path.string()));

  auto frame = udp_audio::audio::make_silent_frame(7);
  frame.samples[0] = 0.25F;
  writer.write_frame(frame);
  writer.close();

  assert(writer.recorded_frames() == 1);
  assert(writer.data_bytes() == udp_audio::audio::kFramePayloadBytes);

  std::ifstream input(path, std::ios::binary);
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
  assert(bytes.size() == 44U + udp_audio::audio::kFramePayloadBytes);
  assert(std::string(reinterpret_cast<const char*>(bytes.data()), 4) == "RIFF");
  assert(std::string(reinterpret_cast<const char*>(bytes.data() + 8), 4) == "WAVE");
  assert(std::string(reinterpret_cast<const char*>(bytes.data() + 36), 4) == "data");
  assert(read_u32_le(bytes, 40) == udp_audio::audio::kFramePayloadBytes);

  std::filesystem::remove(path);
}

void frame_playback_queue_renders_frames_and_counts_underruns() {
  udp_audio::audio::FramePlaybackQueue<4> playback;
  playback.enqueue_preroll(1);

  auto frame = udp_audio::audio::make_silent_frame(1);
  frame.samples.fill(0.5F);
  playback.enqueue_output(frame);

  std::array<float, (udp_audio::audio::kFrameSamples * 2U) + 64U> output{};
  playback.render(output.data(), output.size());

  for (std::size_t i = 0; i < udp_audio::audio::kFrameSamples; ++i) {
    assert(output[i] == 0.0F);
  }
  for (std::size_t i = udp_audio::audio::kFrameSamples;
       i < udp_audio::audio::kFrameSamples * 2U; ++i) {
    assert(output[i] == 0.5F);
  }
  for (std::size_t i = udp_audio::audio::kFrameSamples * 2U; i < output.size(); ++i) {
    assert(output[i] == 0.0F);
  }

  assert(playback.stats().preroll_frames.load(std::memory_order_relaxed) == 1);
  assert(playback.stats().enqueued_output_frames.load(std::memory_order_relaxed) == 1);
  assert(playback.stats().callback_underruns.load(std::memory_order_relaxed) == 1);
  assert(playback.stats().rendered_device_frames.load(std::memory_order_relaxed) ==
         output.size());
}

}  // namespace

int test_audio_io_main() {
  wav_writer_patches_float_pcm_header();
  frame_playback_queue_renders_frames_and_counts_underruns();
  return 0;
}
