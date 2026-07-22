#include "udp_audio/audio/frame.hpp"
#include "udp_audio/concurrency/spsc_ring_buffer.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>

namespace {

using udp_audio::audio::MonoAudioFrame;

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 440.0;
constexpr float kToneGain = 0.2F;
constexpr std::uint32_t kDefaultDurationSeconds = 3;
constexpr std::size_t kQueueCapacityFrames = 128;

struct PlaybackStats {
  std::atomic<std::uint64_t> produced_frames{0};
  std::atomic<std::uint64_t> rendered_device_frames{0};
  std::atomic<std::uint64_t> queue_underruns{0};
  std::atomic<std::uint64_t> dropped_producer_frames{0};
};

struct PlaybackState {
  udp_audio::concurrency::SpscRingBuffer<MonoAudioFrame, kQueueCapacityFrames> queue;
  MonoAudioFrame current_frame{};
  std::size_t current_offset = udp_audio::audio::kFrameSamples;
  PlaybackStats stats{};
};

std::uint32_t parse_duration_seconds(int argc, char** argv) {
  if (argc < 2) {
    return kDefaultDurationSeconds;
  }

  std::uint32_t duration = 0;
  const std::string_view input(argv[1]);
  const auto result = std::from_chars(input.data(), input.data() + input.size(), duration);
  if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || duration == 0) {
    std::cerr << "Usage: udp_audio_miniaudio_playback [duration_seconds]\n";
    return kDefaultDurationSeconds;
  }

  return duration;
}

MonoAudioFrame make_sine_frame(std::uint32_t sequence, double& phase) {
  MonoAudioFrame frame{};
  frame.sequence = sequence;
  frame.timestamp_samples =
    sequence * static_cast<std::uint32_t>(udp_audio::audio::kFrameSamples);

  const double phase_step =
    (2.0 * kPi * kToneHz) / static_cast<double>(udp_audio::audio::kSampleRateHz);

  for (float& sample : frame.samples) {
    sample = static_cast<float>(std::sin(phase)) * kToneGain;
    phase += phase_step;
    if (phase >= 2.0 * kPi) {
      phase -= 2.0 * kPi;
    }
  }

  return frame;
}

void push_sine_frame(PlaybackState& state, std::uint32_t sequence, double& phase) {
  auto frame = make_sine_frame(sequence, phase);
  if (state.queue.push(frame)) {
    state.stats.produced_frames.fetch_add(1, std::memory_order_relaxed);
  } else {
    state.stats.dropped_producer_frames.fetch_add(1, std::memory_order_relaxed);
  }
}

void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
  static_cast<void>(input);

  auto* state = static_cast<PlaybackState*>(device->pUserData);
  auto* out = static_cast<float*>(output);
  ma_uint32 written = 0;

  while (written < frame_count) {
    if (state->current_offset >= udp_audio::audio::kFrameSamples) {
      auto next_frame = state->queue.pop();
      if (!next_frame.has_value()) {
        const auto remaining = static_cast<std::size_t>(frame_count - written);
        std::fill(out + written, out + written + remaining, 0.0F);
        state->stats.queue_underruns.fetch_add(1, std::memory_order_relaxed);
        state->stats.rendered_device_frames.fetch_add(remaining, std::memory_order_relaxed);
        return;
      }

      state->current_frame = *next_frame;
      state->current_offset = 0;
    }

    const auto available_in_frame = udp_audio::audio::kFrameSamples - state->current_offset;
    const auto remaining_output = static_cast<std::size_t>(frame_count - written);
    const auto samples_to_copy = std::min(available_in_frame, remaining_output);

    const auto source = state->current_frame.samples.data() + state->current_offset;
    std::copy(source, source + samples_to_copy, out + written);

    written += static_cast<ma_uint32>(samples_to_copy);
    state->current_offset += samples_to_copy;
    state->stats.rendered_device_frames.fetch_add(samples_to_copy, std::memory_order_relaxed);
  }
}

void produce_sine_frames(PlaybackState& state,
                         std::atomic<bool>& running,
                         std::uint32_t duration_seconds,
                         std::uint32_t start_sequence,
                         double start_phase) {
  double phase = start_phase;
  std::uint32_t sequence = start_sequence;
  const auto start = std::chrono::steady_clock::now();
  auto next_frame_time = start;
  const auto stop_time = start + std::chrono::seconds(duration_seconds);

  while (running.load(std::memory_order_relaxed) &&
         std::chrono::steady_clock::now() < stop_time) {
    std::this_thread::sleep_until(next_frame_time);

    push_sine_frame(state, sequence, phase);
    ++sequence;
    next_frame_time += std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs);
  }

  running.store(false, std::memory_order_relaxed);
}

}  // namespace

int main(int argc, char** argv) {
  const auto duration_seconds = parse_duration_seconds(argc, argv);

  PlaybackState state{};
  std::atomic<bool> running{true};
  double phase = 0.0;
  std::uint32_t next_sequence = 0;

  for (std::size_t i = 0; i < 4; ++i) {
    push_sine_frame(state, next_sequence, phase);
    ++next_sequence;
  }

  auto config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = static_cast<ma_uint32>(udp_audio::audio::kChannels);
  config.sampleRate = udp_audio::audio::kSampleRateHz;
  config.dataCallback = data_callback;
  config.pUserData = &state;

  ma_device device{};
  if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
    std::cerr << "Failed to open playback device\n";
    return 1;
  }

  std::thread producer(produce_sine_frames, std::ref(state), std::ref(running), duration_seconds,
                       next_sequence, phase);

  if (ma_device_start(&device) != MA_SUCCESS) {
    running.store(false, std::memory_order_relaxed);
    producer.join();
    ma_device_uninit(&device);
    std::cerr << "Failed to start playback device\n";
    return 1;
  }

  std::cout << "Playing " << duration_seconds << " seconds of 440 Hz mono float PCM at "
            << udp_audio::audio::kSampleRateHz << " Hz\n";
  std::cout << "Audio callback pulls device frames; producer pushes 10 ms blocks.\n";

  producer.join();
  ma_device_stop(&device);
  ma_device_uninit(&device);

  std::cout << "\nsummary\n";
  std::cout << "produced_10ms_frames="
            << state.stats.produced_frames.load(std::memory_order_relaxed) << '\n';
  std::cout << "rendered_device_frames="
            << state.stats.rendered_device_frames.load(std::memory_order_relaxed) << '\n';
  std::cout << "queue_underruns="
            << state.stats.queue_underruns.load(std::memory_order_relaxed) << '\n';
  std::cout << "dropped_producer_frames="
            << state.stats.dropped_producer_frames.load(std::memory_order_relaxed) << '\n';
}
