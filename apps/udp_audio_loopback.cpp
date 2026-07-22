#include "udp_audio/audio/frame.hpp"
#include "udp_audio/audio/source.hpp"
#include "udp_audio/concurrency/spsc_ring_buffer.hpp"
#include "udp_audio/dsp/plc.hpp"
#include "udp_audio/jitter/adaptive_jitter_buffer.hpp"
#include "udp_audio/jitter/fixed_jitter_buffer.hpp"
#include "udp_audio/protocol/packet.hpp"
#include "udp_audio/transport/udp_socket.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using udp_audio::audio::MonoAudioFrame;

constexpr std::size_t kDefaultFrameCount = 100;
constexpr std::size_t kJitterDepthFrames = 3;
constexpr std::size_t kAdaptiveMaxJitterDepthFrames = 8;
constexpr std::size_t kJitterCapacityFrames = 64;
constexpr std::size_t kPlaybackQueueCapacityFrames = 256;
constexpr std::size_t kPlaybackPrerollFrames = 4;
constexpr std::size_t kPlaybackFadeOutFrames = 1;
constexpr std::size_t kBoundaryCrossfadeSamples = 96;

using PacketBytes =
  std::array<std::byte, udp_audio::protocol::kHeaderSizeBytes + udp_audio::audio::kFramePayloadBytes>;

struct ProgramOptions {
  std::size_t frame_count = kDefaultFrameCount;
  std::uint32_t loss_percent = 0;
  std::uint32_t jitter_ms = 0;
  std::uint32_t seed = 1337;
  bool play_audio = false;
  std::string record_wav_path{};
  std::string plc_mode = "periodic_interp";
  std::string source_mode = "sine";
  std::string jitter_buffer_mode = "fixed";
};

enum class PlcMode {
  none,
  repeat,
  periodic,
  periodic_interp,
};

PlcMode parse_plc_mode(std::string_view value) {
  if (value == "none") {
    return PlcMode::none;
  }
  if (value == "repeat") {
    return PlcMode::repeat;
  }
  if (value == "periodic") {
    return PlcMode::periodic;
  }
  if (value == "periodic_interp") {
    return PlcMode::periodic_interp;
  }
  return PlcMode::periodic_interp;
}

enum class JitterBufferMode {
  fixed,
  adaptive,
};

udp_audio::audio::SourceMode parse_source_mode(std::string_view value) {
  if (value == "chirp") {
    return udp_audio::audio::SourceMode::chirp;
  }
  return udp_audio::audio::SourceMode::sine;
}

JitterBufferMode parse_jitter_buffer_mode(std::string_view value) {
  if (value == "adaptive") {
    return JitterBufferMode::adaptive;
  }
  return JitterBufferMode::fixed;
}

struct PlaybackStats {
  std::atomic<std::uint64_t> preroll_frames{0};
  std::atomic<std::uint64_t> fadeout_frames{0};
  std::atomic<std::uint64_t> enqueued_output_frames{0};
  std::atomic<std::uint64_t> rendered_device_frames{0};
  std::atomic<std::uint64_t> callback_underruns{0};
  std::atomic<std::uint64_t> dropped_output_frames{0};
};

struct PlaybackState {
  udp_audio::concurrency::SpscRingBuffer<MonoAudioFrame, kPlaybackQueueCapacityFrames> queue;
  MonoAudioFrame current_frame{};
  std::size_t current_offset = udp_audio::audio::kFrameSamples;
  PlaybackStats stats{};
};

struct BoundarySmoother {
  float last_sample = 0.0F;
  bool has_last_sample = false;
  bool previous_was_concealed = false;
};

struct BufferedFrame {
  MonoAudioFrame frame{};
  Clock::time_point arrival_time{};
  double network_latency_ms = 0.0;
  double inter_arrival_ms = 0.0;
};

struct LoopbackStats {
  std::size_t generated = 0;
  std::size_t dropped = 0;
  std::size_t sent = 0;
  std::size_t received = 0;
  std::size_t played = 0;
  std::size_t concealed = 0;
  std::size_t datagrams = 0;
  std::size_t invalid_datagrams = 0;
  std::size_t late_datagrams = 0;
  std::uint32_t previous_sequence = 0;
  bool has_previous_sequence = false;
  bool has_previous_arrival = false;
  Clock::time_point previous_arrival{};
  double latency_sum_ms = 0.0;
  double latency_min_ms = std::numeric_limits<double>::max();
  double latency_max_ms = 0.0;
  double playout_latency_sum_ms = 0.0;
  double playout_latency_max_ms = 0.0;
  double inter_arrival_sum_ms = 0.0;
  double inter_arrival_max_ms = 0.0;
  std::size_t target_depth_min_frames = std::numeric_limits<std::size_t>::max();
  std::size_t target_depth_max_frames = 0;
  double target_depth_sum_frames = 0.0;
  std::size_t target_depth_samples = 0;
};

struct PendingPacket {
  PacketBytes bytes{};
  Clock::time_point deliver_at{};
};

class WavWriter {
 public:
  bool open(const std::string& path) {
    output_.open(path, std::ios::binary);
    if (!output_) {
      return false;
    }

    write_header(0);
    path_ = path;
    return output_.good();
  }

  void write_frame(const MonoAudioFrame& frame) {
    if (!output_) {
      return;
    }

    const auto bytes = static_cast<std::streamsize>(udp_audio::audio::kFramePayloadBytes);
    output_.write(reinterpret_cast<const char*>(frame.samples.data()), bytes);
    data_bytes_ += udp_audio::audio::kFramePayloadBytes;
    ++recorded_frames_;
  }

  void close() {
    if (!output_) {
      return;
    }

    output_.seekp(0, std::ios::beg);
    write_header(data_bytes_);
    output_.close();
  }

  [[nodiscard]] std::uint64_t recorded_frames() const noexcept {
    return recorded_frames_;
  }

  [[nodiscard]] std::uint64_t data_bytes() const noexcept {
    return data_bytes_;
  }

  [[nodiscard]] const std::string& path() const noexcept {
    return path_;
  }

 private:
  void write_u16(std::uint16_t value) {
    const char bytes[] = {
      static_cast<char>(value & 0xffU),
      static_cast<char>((value >> 8U) & 0xffU),
    };
    output_.write(bytes, sizeof(bytes));
  }

  void write_u32(std::uint32_t value) {
    const char bytes[] = {
      static_cast<char>(value & 0xffU),
      static_cast<char>((value >> 8U) & 0xffU),
      static_cast<char>((value >> 16U) & 0xffU),
      static_cast<char>((value >> 24U) & 0xffU),
    };
    output_.write(bytes, sizeof(bytes));
  }

  void write_header(std::uint64_t data_bytes) {
    constexpr std::uint16_t kAudioFormatIeeeFloat = 3;
    constexpr std::uint16_t kBitsPerSample = 32;
    constexpr std::uint16_t kBlockAlign =
      static_cast<std::uint16_t>(udp_audio::audio::kChannels * sizeof(float));
    constexpr std::uint32_t kByteRate = udp_audio::audio::kSampleRateHz * kBlockAlign;

    const auto clamped_data_bytes =
      static_cast<std::uint32_t>(std::min<std::uint64_t>(data_bytes, 0xfffffff0ULL));

    output_.write("RIFF", 4);
    write_u32(36U + clamped_data_bytes);
    output_.write("WAVE", 4);

    output_.write("fmt ", 4);
    write_u32(16);
    write_u16(kAudioFormatIeeeFloat);
    write_u16(static_cast<std::uint16_t>(udp_audio::audio::kChannels));
    write_u32(udp_audio::audio::kSampleRateHz);
    write_u32(kByteRate);
    write_u16(kBlockAlign);
    write_u16(kBitsPerSample);

    output_.write("data", 4);
    write_u32(clamped_data_bytes);
  }

  std::ofstream output_{};
  std::string path_{};
  std::uint64_t data_bytes_ = 0;
  std::uint64_t recorded_frames_ = 0;
};

template <typename T>
bool parse_unsigned_arg(std::string_view input, T& value) {
  const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
  return result.ec == std::errc{} && result.ptr == input.data() + input.size();
}

ProgramOptions parse_options(int argc, char** argv) {
  ProgramOptions options{};

  if (argc > 1 && !parse_unsigned_arg(std::string_view(argv[1]), options.frame_count)) {
    std::cerr << "Usage: udp_audio_loopback [frame_count] [loss_percent] [jitter_ms] [seed] "
                 "[play_audio] [record_wav] [plc_mode] [source_mode] [jitter_buffer_mode]\n";
    options.frame_count = kDefaultFrameCount;
  }

  if (argc > 2 && !parse_unsigned_arg(std::string_view(argv[2]), options.loss_percent)) {
    std::cerr << "Usage: udp_audio_loopback [frame_count] [loss_percent] [jitter_ms] [seed] "
                 "[play_audio] [record_wav] [plc_mode] [source_mode] [jitter_buffer_mode]\n";
    options.loss_percent = 0;
  }

  if (argc > 3 && !parse_unsigned_arg(std::string_view(argv[3]), options.jitter_ms)) {
    std::cerr << "Usage: udp_audio_loopback [frame_count] [loss_percent] [jitter_ms] [seed] "
                 "[play_audio] [record_wav] [plc_mode] [source_mode] [jitter_buffer_mode]\n";
    options.jitter_ms = 0;
  }

  if (argc > 4 && !parse_unsigned_arg(std::string_view(argv[4]), options.seed)) {
    std::cerr << "Usage: udp_audio_loopback [frame_count] [loss_percent] [jitter_ms] [seed] "
                 "[play_audio] [record_wav] [plc_mode] [source_mode] [jitter_buffer_mode]\n";
    options.seed = 1337;
  }

  std::uint32_t play_audio = 0;
  if (argc > 5 && !parse_unsigned_arg(std::string_view(argv[5]), play_audio)) {
    std::cerr << "Usage: udp_audio_loopback [frame_count] [loss_percent] [jitter_ms] [seed] "
                 "[play_audio] [record_wav] [plc_mode] [source_mode] [jitter_buffer_mode]\n";
    play_audio = 0;
  }

  if (argc > 6) {
    options.record_wav_path = argv[6];
  }

  if (argc > 7) {
    const std::string_view mode(argv[7]);
    if (mode == "none" || mode == "repeat" || mode == "periodic" ||
        mode == "periodic_interp") {
      options.plc_mode = std::string(mode);
    } else {
      std::cerr << "Unknown PLC mode '" << mode
                << "'. Use none, repeat, periodic, or periodic_interp.\n";
      options.plc_mode = "periodic_interp";
    }
  }

  if (argc > 8) {
    const std::string_view mode(argv[8]);
    if (mode == "sine" || mode == "chirp") {
      options.source_mode = std::string(mode);
    } else {
      std::cerr << "Unknown source mode '" << mode << "'. Use sine or chirp.\n";
      options.source_mode = "sine";
    }
  }

  if (argc > 9) {
    const std::string_view mode(argv[9]);
    if (mode == "fixed" || mode == "adaptive") {
      options.jitter_buffer_mode = std::string(mode);
    } else {
      std::cerr << "Unknown jitter buffer mode '" << mode << "'. Use fixed or adaptive.\n";
      options.jitter_buffer_mode = "fixed";
    }
  }

  if (options.frame_count == 0) {
    options.frame_count = kDefaultFrameCount;
  }
  options.loss_percent = std::min<std::uint32_t>(options.loss_percent, 100);
  options.play_audio = play_audio != 0;
  return options;
}

PacketBytes make_packet(const MonoAudioFrame& frame) {
  using udp_audio::protocol::PacketHeader;

  PacketBytes packet{};

  const PacketHeader header{
    .sequence = frame.sequence,
    .timestamp_samples = frame.timestamp_samples,
    .payload_size = static_cast<std::uint16_t>(udp_audio::audio::kFramePayloadBytes),
  };

  const auto header_bytes = udp_audio::protocol::serialize_header(header);
  std::memcpy(packet.data(), header_bytes.data(), header_bytes.size());
  std::memcpy(packet.data() + header_bytes.size(), frame.samples.data(),
              udp_audio::audio::kFramePayloadBytes);

  return packet;
}

BufferedFrame make_buffered_frame(const udp_audio::protocol::PacketHeader& header,
                                  std::span<const std::byte> payload,
                                  Clock::time_point arrival_time,
                                  const std::vector<Clock::time_point>& sent_times,
                                  udp_audio::jitter::AdaptiveJitterController* adaptive_controller,
                                  LoopbackStats& stats) {
  stats.previous_sequence = header.sequence;
  stats.has_previous_sequence = true;

  double inter_arrival_ms = 0.0;
  if (stats.has_previous_arrival) {
    inter_arrival_ms =
      std::chrono::duration<double, std::milli>(arrival_time - stats.previous_arrival).count();
    stats.inter_arrival_sum_ms += inter_arrival_ms;
    stats.inter_arrival_max_ms = std::max(stats.inter_arrival_max_ms, inter_arrival_ms);
    if (adaptive_controller != nullptr) {
      adaptive_controller->observe_inter_arrival(inter_arrival_ms);
    }
  }

  stats.previous_arrival = arrival_time;
  stats.has_previous_arrival = true;

  double latency_ms = 0.0;
  if (header.sequence < sent_times.size()) {
    latency_ms =
      std::chrono::duration<double, std::milli>(arrival_time - sent_times[header.sequence]).count();
    stats.latency_sum_ms += latency_ms;
    stats.latency_min_ms = std::min(stats.latency_min_ms, latency_ms);
    stats.latency_max_ms = std::max(stats.latency_max_ms, latency_ms);
  }

  ++stats.received;

  BufferedFrame buffered{};
  buffered.frame.sequence = header.sequence;
  buffered.frame.timestamp_samples = header.timestamp_samples;
  buffered.arrival_time = arrival_time;
  buffered.network_latency_ms = latency_ms;
  buffered.inter_arrival_ms = inter_arrival_ms;
  std::memcpy(buffered.frame.samples.data(), payload.data(), udp_audio::audio::kFramePayloadBytes);
  return buffered;
}

bool drain_receiver(udp_audio::transport::UdpSocket& receiver,
                    std::vector<Clock::time_point>& sent_times,
                    udp_audio::jitter::FixedJitterBuffer<BufferedFrame, kJitterCapacityFrames>&
                      jitter_buffer,
                    std::uint32_t next_play_sequence,
                    udp_audio::jitter::AdaptiveJitterController* adaptive_controller,
                    LoopbackStats& stats,
                    std::error_code& error) {
  std::array<std::byte, 2048> buffer{};

  while (true) {
    const auto result = receiver.receive_from(buffer, error);
    if (error) {
      return false;
    }

    if (!result.has_value()) {
      return true;
    }

    ++stats.datagrams;

    if (result->bytes_received < udp_audio::protocol::kHeaderSizeBytes) {
      ++stats.invalid_datagrams;
      continue;
    }

    const auto header = udp_audio::protocol::parse_header(
      std::span<const std::byte, udp_audio::protocol::kHeaderSizeBytes>(
        buffer.data(), udp_audio::protocol::kHeaderSizeBytes));

    if (!udp_audio::protocol::is_valid_header(header) ||
        header.payload_size != udp_audio::audio::kFramePayloadBytes ||
        result->bytes_received <
          udp_audio::protocol::kHeaderSizeBytes + udp_audio::audio::kFramePayloadBytes) {
      ++stats.invalid_datagrams;
      continue;
    }

    if (header.sequence < next_play_sequence) {
      ++stats.late_datagrams;
      continue;
    }

    const auto payload = std::span<const std::byte>(
      buffer.data() + udp_audio::protocol::kHeaderSizeBytes, udp_audio::audio::kFramePayloadBytes);
    const auto buffered =
      make_buffered_frame(header, payload, Clock::now(), sent_times, adaptive_controller, stats);
    static_cast<void>(jitter_buffer.push(header.sequence, buffered));
  }
}

bool release_due_packets(std::deque<PendingPacket>& pending_packets,
                         udp_audio::transport::UdpSocket& sender,
                         const udp_audio::transport::Endpoint& receiver_endpoint,
                         LoopbackStats& stats,
                         std::error_code& error) {
  const auto now = Clock::now();
  auto packet = pending_packets.begin();

  while (packet != pending_packets.end()) {
    if (packet->deliver_at > now) {
      ++packet;
      continue;
    }

    const auto sent = sender.send_to(packet->bytes, receiver_endpoint, error);
    if (error || sent != packet->bytes.size()) {
      return false;
    }

    ++stats.sent;
    packet = pending_packets.erase(packet);
  }

  return true;
}

void schedule_packet(PacketBytes packet,
                     const ProgramOptions& options,
                     std::mt19937& rng,
                     std::deque<PendingPacket>& pending_packets,
                     LoopbackStats& stats) {
  ++stats.generated;

  std::uniform_int_distribution<std::uint32_t> loss_distribution(1, 100);
  if (options.loss_percent > 0 && loss_distribution(rng) <= options.loss_percent) {
    ++stats.dropped;
    return;
  }

  std::uint32_t delay_ms = 0;
  if (options.jitter_ms > 0) {
    std::uniform_int_distribution<std::uint32_t> jitter_distribution(0, options.jitter_ms);
    delay_ms = jitter_distribution(rng);
  }

  pending_packets.push_back(PendingPacket{
    .bytes = packet,
    .deliver_at = Clock::now() + std::chrono::milliseconds(delay_ms),
  });
}

void playback_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
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
        state->stats.callback_underruns.fetch_add(1, std::memory_order_relaxed);
        state->stats.rendered_device_frames.fetch_add(remaining, std::memory_order_relaxed);
        return;
      }

      state->current_frame = *next_frame;
      state->current_offset = 0;
    }

    const auto available_in_frame = udp_audio::audio::kFrameSamples - state->current_offset;
    const auto remaining_output = static_cast<std::size_t>(frame_count - written);
    const auto samples_to_copy = std::min(available_in_frame, remaining_output);
    const auto* source = state->current_frame.samples.data() + state->current_offset;

    std::copy(source, source + samples_to_copy, out + written);

    written += static_cast<ma_uint32>(samples_to_copy);
    state->current_offset += samples_to_copy;
    state->stats.rendered_device_frames.fetch_add(samples_to_copy, std::memory_order_relaxed);
  }
}

void enqueue_for_playback(const MonoAudioFrame& frame, PlaybackState* playback_state) {
  if (playback_state == nullptr) {
    return;
  }

  if (playback_state->queue.push(frame)) {
    playback_state->stats.enqueued_output_frames.fetch_add(1, std::memory_order_relaxed);
  } else {
    playback_state->stats.dropped_output_frames.fetch_add(1, std::memory_order_relaxed);
  }
}

void smooth_boundary_if_needed(MonoAudioFrame& frame,
                               BoundarySmoother& smoother,
                               bool current_is_concealed,
                               bool force_smooth = false,
                               bool allow_smooth = true) {
  const bool should_smooth =
    allow_smooth &&
    smoother.has_last_sample &&
    (force_smooth || current_is_concealed || smoother.previous_was_concealed);

  if (should_smooth) {
    const float correction = smoother.last_sample - frame.samples[0];
    for (std::size_t i = 0; i < kBoundaryCrossfadeSamples; ++i) {
      const auto fade = 1.0F - (static_cast<float>(i) /
                                static_cast<float>(kBoundaryCrossfadeSamples - 1U));
      frame.samples[i] += correction * fade;
    }
  }

  smoother.last_sample = frame.samples.back();
  smoother.has_last_sample = true;
  smoother.previous_was_concealed = current_is_concealed;
}

void enqueue_fadeout_frame(BoundarySmoother& smoother, PlaybackState* playback_state) {
  if (playback_state == nullptr) {
    return;
  }

  auto fadeout_frame = udp_audio::audio::make_silent_frame(0);
  smooth_boundary_if_needed(fadeout_frame, smoother, true, true);

  if (playback_state->queue.push(fadeout_frame)) {
    playback_state->stats.fadeout_frames.fetch_add(1, std::memory_order_relaxed);
  } else {
    playback_state->stats.dropped_output_frames.fetch_add(1, std::memory_order_relaxed);
  }
}

void synthesize_missing_frame(PlcMode mode,
                              udp_audio::dsp::HoldAndDecayPlc<udp_audio::audio::kFrameSamples>& plc,
                              MonoAudioFrame& concealed_frame) {
  if (mode == PlcMode::none) {
    return;
  }

  plc.synthesize_missing_frame(
    std::span<float, udp_audio::audio::kFrameSamples>(concealed_frame.samples),
    mode == PlcMode::periodic_interp
      ? udp_audio::dsp::PlcSynthesisMode::periodic_interpolated
      : (mode == PlcMode::periodic ? udp_audio::dsp::PlcSynthesisMode::periodic
                                    : udp_audio::dsp::PlcSynthesisMode::repeat_last_frame));
}

void record_target_depth(std::size_t target_depth_frames, LoopbackStats& stats) {
  stats.target_depth_min_frames =
    std::min(stats.target_depth_min_frames, target_depth_frames);
  stats.target_depth_max_frames =
    std::max(stats.target_depth_max_frames, target_depth_frames);
  stats.target_depth_sum_frames += static_cast<double>(target_depth_frames);
  ++stats.target_depth_samples;
}

void play_expected_frame(std::uint32_t sequence,
                         udp_audio::jitter::FixedJitterBuffer<BufferedFrame,
                                                               kJitterCapacityFrames>& jitter_buffer,
                         udp_audio::dsp::HoldAndDecayPlc<udp_audio::audio::kFrameSamples>& plc,
                         PlcMode plc_mode,
                         std::size_t target_depth_frames,
                         BoundarySmoother& smoother,
                         PlaybackState* playback_state,
                         WavWriter* recorder,
                         const std::vector<Clock::time_point>& sent_times,
                         LoopbackStats& stats) {
  const auto now = Clock::now();
  const double playout_latency_ms =
    sequence < sent_times.size()
      ? std::chrono::duration<double, std::milli>(now - sent_times[sequence]).count()
      : 0.0;

  auto frame = jitter_buffer.pop_expected(sequence);
  if (!frame.has_value()) {
    MonoAudioFrame concealed_frame{};
    concealed_frame.sequence = sequence;
    concealed_frame.timestamp_samples =
      sequence * static_cast<std::uint32_t>(udp_audio::audio::kFrameSamples);
    synthesize_missing_frame(plc_mode, plc, concealed_frame);

    stats.playout_latency_sum_ms += playout_latency_ms;
    stats.playout_latency_max_ms = std::max(stats.playout_latency_max_ms, playout_latency_ms);
    ++stats.concealed;
    const bool uses_plc_synthesis = plc_mode != PlcMode::none;
    smooth_boundary_if_needed(concealed_frame, smoother, true, false, !uses_plc_synthesis);
    if (recorder != nullptr) {
      recorder->write_frame(concealed_frame);
    }
    enqueue_for_playback(concealed_frame, playback_state);

    std::cout << sequence << ',' << concealed_frame.timestamp_samples << ",,"
              << playout_latency_ms << ",," << target_depth_frames << ",concealed\n";
    return;
  }

  stats.playout_latency_sum_ms += playout_latency_ms;
  stats.playout_latency_max_ms = std::max(stats.playout_latency_max_ms, playout_latency_ms);
  ++stats.played;
  auto output_frame = frame->frame;
  const bool uses_plc_synthesis = plc_mode != PlcMode::none;
  plc.accept_good_frame(
    std::span<const float, udp_audio::audio::kFrameSamples>(frame->frame.samples));
  smooth_boundary_if_needed(output_frame, smoother, false, false, !uses_plc_synthesis);
  if (recorder != nullptr) {
    recorder->write_frame(output_frame);
  }
  enqueue_for_playback(output_frame, playback_state);

  std::cout << sequence << ',' << frame->frame.timestamp_samples << ','
            << frame->network_latency_ms << ',' << playout_latency_ms << ','
            << frame->inter_arrival_ms << ',' << target_depth_frames << ",played\n";
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  const auto frame_count = options.frame_count;

  std::error_code error;
  auto receiver = udp_audio::transport::UdpSocket::open_ipv4(error);
  if (error || !receiver.valid()) {
    std::cerr << "Failed to open receiver socket: " << error.message() << '\n';
    return 1;
  }

  if (!receiver.bind(udp_audio::transport::Endpoint::loopback(0), error)) {
    std::cerr << "Failed to bind receiver socket: " << error.message() << '\n';
    return 1;
  }

  const auto bound_endpoint = receiver.local_endpoint(error);
  if (error || !bound_endpoint.has_value()) {
    std::cerr << "Failed to inspect receiver endpoint: " << error.message() << '\n';
    return 1;
  }
  const auto receiver_endpoint = udp_audio::transport::Endpoint::loopback(bound_endpoint->port);

  auto sender = udp_audio::transport::UdpSocket::open_ipv4(error);
  if (error || !sender.valid()) {
    std::cerr << "Failed to open sender socket: " << error.message() << '\n';
    return 1;
  }

  std::vector<Clock::time_point> sent_times(frame_count);
  const auto plc_mode = parse_plc_mode(options.plc_mode);
  const auto source_mode = parse_source_mode(options.source_mode);
  const auto jitter_buffer_mode = parse_jitter_buffer_mode(options.jitter_buffer_mode);
  udp_audio::jitter::FixedJitterBuffer<BufferedFrame, kJitterCapacityFrames> jitter_buffer(
    kJitterDepthFrames);
  udp_audio::jitter::AdaptiveJitterController adaptive_jitter_controller(
    kJitterDepthFrames, kAdaptiveMaxJitterDepthFrames,
    static_cast<double>(udp_audio::audio::kFrameDurationMs));
  auto* adaptive_jitter =
    jitter_buffer_mode == JitterBufferMode::adaptive ? &adaptive_jitter_controller : nullptr;
  udp_audio::dsp::HoldAndDecayPlc<udp_audio::audio::kFrameSamples> plc;
  BoundarySmoother boundary_smoother{};
  WavWriter recorder{};
  WavWriter* active_recorder = nullptr;
  PlaybackState playback_state{};
  PlaybackState* playback = options.play_audio ? &playback_state : nullptr;
  ma_device playback_device{};
  bool playback_device_initialized = false;
  bool playback_started = false;

  if (options.play_audio) {
    const auto preroll_frame = udp_audio::audio::make_silent_frame(0);
    for (std::size_t i = 0; i < kPlaybackPrerollFrames; ++i) {
      if (playback_state.queue.push(preroll_frame)) {
        playback_state.stats.preroll_frames.fetch_add(1, std::memory_order_relaxed);
      }
    }

    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(udp_audio::audio::kChannels);
    config.sampleRate = udp_audio::audio::kSampleRateHz;
    config.dataCallback = playback_callback;
    config.pUserData = &playback_state;

    if (ma_device_init(nullptr, &config, &playback_device) != MA_SUCCESS) {
      std::cerr << "Failed to open playback device\n";
      return 1;
    }
    playback_device_initialized = true;
    if (ma_device_start(&playback_device) != MA_SUCCESS) {
      std::cerr << "Failed to start playback device\n";
      ma_device_uninit(&playback_device);
      return 1;
    }
    playback_started = true;
  }

  if (!options.record_wav_path.empty()) {
    if (!recorder.open(options.record_wav_path)) {
      std::cerr << "Failed to open recording WAV: " << options.record_wav_path << '\n';
      if (playback_started) {
        ma_device_stop(&playback_device);
      }
      if (playback_device_initialized) {
        ma_device_uninit(&playback_device);
      }
      return 1;
    }
    active_recorder = &recorder;
  }

  LoopbackStats stats{};
  std::deque<PendingPacket> pending_packets;
  std::mt19937 rng(options.seed);
  udp_audio::audio::SourceState source_state{};
  const auto stream_start = Clock::now();
  auto next_send_time = stream_start;
  std::uint32_t next_play_sequence = 0;

  const auto current_target_depth = [&]() {
    return jitter_buffer_mode == JitterBufferMode::adaptive
             ? adaptive_jitter_controller.stats().target_depth_frames
             : kJitterDepthFrames;
  };

  std::cout << "sequence,timestamp_samples,network_latency_ms,playout_latency_ms,"
               "inter_arrival_ms,jitter_depth_frames,status\n";

  for (std::size_t i = 0; i < frame_count; ++i) {
    std::this_thread::sleep_until(next_send_time);

    const auto frame =
      udp_audio::audio::make_source_frame(source_mode, static_cast<std::uint32_t>(i),
                                          frame_count, source_state);
    const auto packet = make_packet(frame);
    sent_times[i] = Clock::now();

    schedule_packet(packet, options, rng, pending_packets, stats);

    if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
      std::cerr << "Failed to send due packets: " << error.message() << '\n';
      return 1;
    }

    const auto receive_until = Clock::now() + std::chrono::milliseconds(2);
    while (Clock::now() < receive_until) {
      if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
        std::cerr << "Failed to send due packets while receiving: " << error.message() << '\n';
        return 1;
      }
      if (!drain_receiver(receiver, sent_times, jitter_buffer, next_play_sequence,
                          adaptive_jitter, stats, error)) {
        std::cerr << "Failed to receive packet: " << error.message() << '\n';
        return 1;
      }
      if (pending_packets.empty() && stats.received >= stats.sent) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    const auto target_depth_frames = current_target_depth();
    record_target_depth(target_depth_frames, stats);
    if (next_play_sequence + target_depth_frames <= i) {
      play_expected_frame(next_play_sequence, jitter_buffer, plc, plc_mode, target_depth_frames,
                          boundary_smoother, playback, active_recorder, sent_times, stats);
      ++next_play_sequence;
    }

    next_send_time += std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs);
  }

  auto tail_play_time = next_send_time;
  while (next_play_sequence < frame_count) {
    std::this_thread::sleep_until(tail_play_time);
    if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
      std::cerr << "Failed to send delayed packets before tail playout: " << error.message()
                << '\n';
      return 1;
    }
    if (!drain_receiver(receiver, sent_times, jitter_buffer, next_play_sequence,
                        adaptive_jitter, stats, error)) {
      std::cerr << "Failed to drain receiver before tail playout: " << error.message() << '\n';
      return 1;
    }
    const auto target_depth_frames = current_target_depth();
    record_target_depth(target_depth_frames, stats);
    play_expected_frame(next_play_sequence, jitter_buffer, plc, plc_mode, target_depth_frames,
                        boundary_smoother, playback, active_recorder, sent_times, stats);
    ++next_play_sequence;
    tail_play_time += std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs);
  }

  const auto final_drain_until = Clock::now() + std::chrono::milliseconds(50 + options.jitter_ms);
  while (Clock::now() < final_drain_until && !pending_packets.empty()) {
    if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
      std::cerr << "Failed to release late tail packets: " << error.message() << '\n';
      return 1;
    }
    if (!drain_receiver(receiver, sent_times, jitter_buffer, next_play_sequence,
                        adaptive_jitter, stats, error)) {
      std::cerr << "Failed to drain late tail packets: " << error.message() << '\n';
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (std::size_t i = 0; i < 3; ++i) {
    if (!drain_receiver(receiver, sent_times, jitter_buffer, next_play_sequence,
                        adaptive_jitter, stats, error)) {
      std::cerr << "Failed to settle late tail packets: " << error.message() << '\n';
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (playback != nullptr) {
    for (std::size_t i = 0; i < kPlaybackFadeOutFrames; ++i) {
      enqueue_fadeout_frame(boundary_smoother, playback);
    }
  }

  if (playback_started) {
    const auto expected_device_frames =
      (stats.played + stats.concealed + kPlaybackPrerollFrames + kPlaybackFadeOutFrames) *
      udp_audio::audio::kFrameSamples;
    const auto playback_deadline = Clock::now() + std::chrono::milliseconds(
                                                  250 + 10 * static_cast<int>(
                                                          kAdaptiveMaxJitterDepthFrames));
    while (Clock::now() < playback_deadline &&
           playback_state.stats.rendered_device_frames.load(std::memory_order_relaxed) <
             expected_device_frames) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ma_device_stop(&playback_device);
  }

  if (playback_device_initialized) {
    ma_device_uninit(&playback_device);
  }

  recorder.close();

  const auto average_latency =
    stats.received == 0 ? 0.0 : stats.latency_sum_ms / static_cast<double>(stats.received);
  const auto output_frames = stats.played + stats.concealed;
  const auto average_playout_latency =
    output_frames == 0 ? 0.0 : stats.playout_latency_sum_ms / static_cast<double>(output_frames);
  const auto inter_arrival_samples = stats.received > 1 ? stats.received - 1U : 0U;
  const auto average_inter_arrival =
    inter_arrival_samples == 0
      ? 0.0
      : stats.inter_arrival_sum_ms / static_cast<double>(inter_arrival_samples);
  const auto average_target_depth =
    stats.target_depth_samples == 0
      ? 0.0
      : stats.target_depth_sum_frames / static_cast<double>(stats.target_depth_samples);
  const auto min_target_depth =
    stats.target_depth_samples == 0 ? 0U : stats.target_depth_min_frames;
  const auto adaptive_stats = adaptive_jitter_controller.stats();

  std::cout << "\nsummary\n";
  std::cout << "receiver=" << receiver_endpoint.address << ':' << receiver_endpoint.port << '\n';
  std::cout << "configured_loss_percent=" << options.loss_percent << '\n';
  std::cout << "configured_jitter_ms=" << options.jitter_ms << '\n';
  std::cout << "seed=" << options.seed << '\n';
  std::cout << "play_audio=" << (options.play_audio ? 1 : 0) << '\n';
  std::cout << "record_wav=" << (options.record_wav_path.empty() ? "(none)" : options.record_wav_path)
            << '\n';
  std::cout << "source_mode=" << options.source_mode << '\n';
  std::cout << "jitter_buffer_mode=" << options.jitter_buffer_mode << '\n';
  std::cout << "generated=" << stats.generated << '\n';
  std::cout << "dropped_by_impairment=" << stats.dropped << '\n';
  std::cout << "sent=" << stats.sent << '\n';
  std::cout << "received=" << stats.received << '\n';
  std::cout << "played=" << stats.played << '\n';
  std::cout << "concealed=" << stats.concealed << '\n';
  std::cout << "output_frames=" << output_frames << '\n';
  std::cout << "datagrams=" << stats.datagrams << '\n';
  std::cout << "invalid_datagrams=" << stats.invalid_datagrams << '\n';
  std::cout << "late_datagrams=" << stats.late_datagrams << '\n';
  std::cout << "jitter_depth_frames=" << current_target_depth() << '\n';
  std::cout << "jitter_depth_min_frames=" << min_target_depth << '\n';
  std::cout << "jitter_depth_max_frames=" << stats.target_depth_max_frames << '\n';
  std::cout << "avg_jitter_depth_frames=" << average_target_depth << '\n';
  std::cout << "jitter_underruns=" << jitter_buffer.stats().underruns << '\n';
  std::cout << "missing_frames=" << jitter_buffer.stats().underruns << '\n';
  if (jitter_buffer_mode == JitterBufferMode::adaptive) {
    std::cout << "adaptive_observations=" << adaptive_stats.observations << '\n';
    std::cout << "adaptive_mean_inter_arrival_ms="
              << adaptive_stats.mean_inter_arrival_ms << '\n';
    std::cout << "adaptive_mean_abs_deviation_ms="
              << adaptive_stats.mean_abs_deviation_ms << '\n';
    std::cout << "adaptive_variance_inter_arrival_ms="
              << adaptive_stats.variance_inter_arrival_ms << '\n';
  }
  std::cout << "plc_mode=" << options.plc_mode << '\n';
  std::cout << "plc_boundary_crossfade_samples=" << kBoundaryCrossfadeSamples << '\n';
  std::cout << "avg_network_latency_ms=" << average_latency << '\n';
  std::cout << "min_network_latency_ms=" << (stats.received == 0 ? 0.0 : stats.latency_min_ms)
            << '\n';
  std::cout << "max_network_latency_ms=" << stats.latency_max_ms << '\n';
  std::cout << "avg_playout_latency_ms=" << average_playout_latency << '\n';
  std::cout << "max_playout_latency_ms=" << stats.playout_latency_max_ms << '\n';
  std::cout << "avg_inter_arrival_ms=" << average_inter_arrival << '\n';
  std::cout << "max_inter_arrival_ms=" << stats.inter_arrival_max_ms << '\n';
  if (!options.record_wav_path.empty()) {
    std::cout << "recorded_output_frames=" << recorder.recorded_frames() << '\n';
    std::cout << "recorded_data_bytes=" << recorder.data_bytes() << '\n';
  }
  if (options.play_audio) {
    std::cout << "audio_preroll_frames="
              << playback_state.stats.preroll_frames.load(std::memory_order_relaxed) << '\n';
    std::cout << "audio_fadeout_frames="
              << playback_state.stats.fadeout_frames.load(std::memory_order_relaxed) << '\n';
    std::cout << "audio_enqueued_output_frames="
              << playback_state.stats.enqueued_output_frames.load(std::memory_order_relaxed)
              << '\n';
    std::cout << "audio_rendered_device_frames="
              << playback_state.stats.rendered_device_frames.load(std::memory_order_relaxed)
              << '\n';
    std::cout << "audio_callback_underruns="
              << playback_state.stats.callback_underruns.load(std::memory_order_relaxed) << '\n';
    std::cout << "audio_dropped_output_frames="
              << playback_state.stats.dropped_output_frames.load(std::memory_order_relaxed)
              << '\n';
  }
}
