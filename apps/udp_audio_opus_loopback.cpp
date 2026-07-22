#include "udp_audio/audio/frame.hpp"
#include "udp_audio/concurrency/spsc_ring_buffer.hpp"
#include "udp_audio/jitter/fixed_jitter_buffer.hpp"
#include "udp_audio/protocol/packet.hpp"
#include "udp_audio/transport/udp_socket.hpp"

#include "miniaudio.h"

#include <opus/opus.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
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

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 440.0;
constexpr double kChirpStartHz = 220.0;
constexpr double kChirpEndHz = 880.0;
constexpr float kToneGain = 0.2F;
constexpr std::size_t kDefaultFrameCount = 100;
constexpr std::size_t kDefaultJitterDepthFrames = 5;
constexpr std::size_t kJitterCapacityFrames = 64;
constexpr std::size_t kPlaybackQueueCapacityFrames = 256;
constexpr std::size_t kPlaybackPrerollFrames = 4;
constexpr std::size_t kPlaybackFadeOutFrames = 1;
constexpr std::size_t kPlaybackFadeOutSamples = 96;
constexpr std::size_t kMaxOpusPacketBytes = 1500;
constexpr std::size_t kMaxRedundantOpusFrames = 3;
constexpr std::size_t kDefaultRedundantOpusFrames = 3;
constexpr std::int32_t kDefaultBitrateBps = 64000;

enum class SourceMode {
  sine,
  chirp,
};

struct ProgramOptions {
  std::size_t frame_count = kDefaultFrameCount;
  std::uint32_t loss_percent = 0;
  std::uint32_t jitter_ms = 0;
  std::uint32_t seed = 1337;
  std::string record_wav_path{};
  std::string source_mode = "sine";
  std::int32_t bitrate_bps = kDefaultBitrateBps;
  std::string recovery_mode = "plc";
  bool play_audio = false;
  std::size_t redundancy_frames = kDefaultRedundantOpusFrames;
  std::size_t jitter_depth_frames = kDefaultJitterDepthFrames;
};

struct EncodedPacket {
  std::array<unsigned char, kMaxOpusPacketBytes> payload{};
  std::size_t payload_size = 0;
  std::uint32_t sequence = 0;
  std::uint32_t timestamp_samples = 0;
};

struct BufferedPacket {
  EncodedPacket packet{};
  std::array<EncodedPacket, kMaxRedundantOpusFrames> redundant_packets{};
  std::size_t redundant_packet_count = 0;
  Clock::time_point arrival_time{};
  double network_latency_ms = 0.0;
  double inter_arrival_ms = 0.0;
};

struct PendingPacket {
  std::vector<std::byte> bytes{};
  Clock::time_point deliver_at{};
};

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

struct LoopbackStats {
  std::size_t generated = 0;
  std::size_t encoded = 0;
  std::size_t dropped = 0;
  std::size_t sent = 0;
  std::size_t received = 0;
  std::size_t decoded = 0;
  std::size_t concealed = 0;
  std::size_t fec_recovered = 0;
  std::size_t plc_generated = 0;
  std::size_t redundancy_recovered = 0;
  std::size_t datagrams = 0;
  std::size_t invalid_datagrams = 0;
  std::size_t opus_decode_errors = 0;
  std::size_t opus_encoded_bytes = 0;
  std::size_t redundancy_bytes = 0;
  bool has_previous_arrival = false;
  Clock::time_point previous_arrival{};
  double latency_sum_ms = 0.0;
  double latency_min_ms = std::numeric_limits<double>::max();
  double latency_max_ms = 0.0;
  double playout_latency_sum_ms = 0.0;
  double playout_latency_max_ms = 0.0;
  double inter_arrival_sum_ms = 0.0;
  double inter_arrival_max_ms = 0.0;
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
  constexpr std::string_view kUsage =
    "Usage: udp_audio_opus_loopback [frame_count] [loss_percent] [jitter_ms] [seed] "
    "[record_wav] [source_mode] [bitrate_bps] [recovery_mode] [play_audio] "
    "[redundancy_frames] [jitter_depth_frames]\n";

  if (argc > 1 && !parse_unsigned_arg(std::string_view(argv[1]), options.frame_count)) {
    std::cerr << kUsage;
    options.frame_count = kDefaultFrameCount;
  }

  if (argc > 2 && !parse_unsigned_arg(std::string_view(argv[2]), options.loss_percent)) {
    std::cerr << kUsage;
    options.loss_percent = 0;
  }

  if (argc > 3 && !parse_unsigned_arg(std::string_view(argv[3]), options.jitter_ms)) {
    std::cerr << kUsage;
    options.jitter_ms = 0;
  }

  if (argc > 4 && !parse_unsigned_arg(std::string_view(argv[4]), options.seed)) {
    std::cerr << kUsage;
    options.seed = 1337;
  }

  if (argc > 5) {
    options.record_wav_path = argv[5];
  }

  if (argc > 6) {
    const std::string_view mode(argv[6]);
    if (mode == "sine" || mode == "chirp") {
      options.source_mode = std::string(mode);
    } else {
      std::cerr << "Unknown source mode '" << mode << "'. Use sine or chirp.\n";
      options.source_mode = "sine";
    }
  }

  if (argc > 7 && !parse_unsigned_arg(std::string_view(argv[7]), options.bitrate_bps)) {
    std::cerr << kUsage;
    options.bitrate_bps = kDefaultBitrateBps;
  }

  if (argc > 8) {
    const std::string_view mode(argv[8]);
    if (mode == "plc" || mode == "fec") {
      options.recovery_mode = std::string(mode);
    } else {
      std::cerr << "Unknown recovery mode '" << mode << "'. Use plc or fec.\n";
      options.recovery_mode = "plc";
    }
  }

  std::uint32_t play_audio = 0;
  if (argc > 9 && !parse_unsigned_arg(std::string_view(argv[9]), play_audio)) {
    std::cerr << kUsage;
    play_audio = 0;
  }

  if (argc > 10 && !parse_unsigned_arg(std::string_view(argv[10]), options.redundancy_frames)) {
    std::cerr << kUsage;
    options.redundancy_frames = kDefaultRedundantOpusFrames;
  }

  if (argc > 11 && !parse_unsigned_arg(std::string_view(argv[11]), options.jitter_depth_frames)) {
    std::cerr << kUsage;
    options.jitter_depth_frames = kDefaultJitterDepthFrames;
  }

  if (options.frame_count == 0) {
    options.frame_count = kDefaultFrameCount;
  }
  options.loss_percent = std::min<std::uint32_t>(options.loss_percent, 100);
  options.bitrate_bps = std::max<std::int32_t>(6000, options.bitrate_bps);
  options.play_audio = play_audio != 0;
  options.redundancy_frames =
    std::min<std::size_t>(options.redundancy_frames, kMaxRedundantOpusFrames);
  options.jitter_depth_frames =
    std::clamp<std::size_t>(options.jitter_depth_frames, 1U, kJitterCapacityFrames / 2U);
  return options;
}

SourceMode parse_source_mode(std::string_view value) {
  if (value == "chirp") {
    return SourceMode::chirp;
  }
  return SourceMode::sine;
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

MonoAudioFrame make_chirp_frame(std::uint32_t sequence,
                                std::size_t total_frames,
                                double& phase) {
  MonoAudioFrame frame{};
  frame.sequence = sequence;
  frame.timestamp_samples =
    sequence * static_cast<std::uint32_t>(udp_audio::audio::kFrameSamples);

  const auto total_samples =
    std::max<std::size_t>(1U, total_frames * udp_audio::audio::kFrameSamples);

  for (std::size_t i = 0; i < frame.samples.size(); ++i) {
    const auto absolute_sample =
      static_cast<std::size_t>(sequence) * udp_audio::audio::kFrameSamples + i;
    const double progress =
      static_cast<double>(std::min(absolute_sample, total_samples - 1U)) /
      static_cast<double>(total_samples - 1U);
    const double frequency = kChirpStartHz + ((kChirpEndHz - kChirpStartHz) * progress);
    const double phase_step =
      (2.0 * kPi * frequency) / static_cast<double>(udp_audio::audio::kSampleRateHz);

    frame.samples[i] = static_cast<float>(std::sin(phase)) * kToneGain;
    phase += phase_step;
    if (phase >= 2.0 * kPi) {
      phase -= 2.0 * kPi;
    }
  }

  return frame;
}

MonoAudioFrame make_source_frame(SourceMode source_mode,
                                 std::uint32_t sequence,
                                 std::size_t total_frames,
                                 double& phase) {
  if (source_mode == SourceMode::chirp) {
    return make_chirp_frame(sequence, total_frames, phase);
  }
  return make_sine_frame(sequence, phase);
}

void append_u8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
  out.push_back(static_cast<std::byte>(value & 0xffU));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
  out.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
  out.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
  out.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
  out.push_back(static_cast<std::byte>(value & 0xffU));
}

std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(
    (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
    static_cast<std::uint16_t>(bytes[offset + 1U]));
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
         static_cast<std::uint32_t>(bytes[offset + 3U]);
}

std::vector<std::byte> make_packet(const EncodedPacket& encoded,
                                   const std::deque<EncodedPacket>& redundancy_history,
                                   std::size_t redundancy_frames,
                                   LoopbackStats& stats) {
  using udp_audio::protocol::PacketHeader;

  constexpr std::uint8_t kBundleMagic = 0x4f;  // "O"
  constexpr std::uint8_t kBundleVersion = 1;

  std::size_t repair_count = 0;
  std::size_t projected_payload_size = 6U + encoded.payload_size;
  const auto requested_repair_count =
    std::min<std::size_t>(redundancy_frames, redundancy_history.size());
  for (std::size_t i = 0; i < requested_repair_count; ++i) {
    const auto& repair = redundancy_history[redundancy_history.size() - 1U - i];
    const auto next_size = projected_payload_size + 10U + repair.payload_size;
    if (next_size > udp_audio::protocol::kMaxPayloadBytes) {
      break;
    }
    projected_payload_size = next_size;
    ++repair_count;
  }
  std::vector<std::byte> payload;
  payload.reserve(projected_payload_size);

  append_u8(payload, kBundleMagic);
  append_u8(payload, kBundleVersion);
  append_u8(payload, static_cast<std::uint8_t>(repair_count));
  append_u8(payload, 0);
  append_u16(payload, static_cast<std::uint16_t>(encoded.payload_size));

  for (std::size_t i = 0; i < repair_count; ++i) {
    const auto& repair = redundancy_history[redundancy_history.size() - 1U - i];
    append_u32(payload, repair.sequence);
    append_u32(payload, repair.timestamp_samples);
    append_u16(payload, static_cast<std::uint16_t>(repair.payload_size));
    stats.redundancy_bytes += repair.payload_size;
  }

  const auto append_packet_payload = [&payload](const EncodedPacket& packet) {
    const auto* begin = reinterpret_cast<const std::byte*>(packet.payload.data());
    payload.insert(payload.end(), begin, begin + packet.payload_size);
  };

  append_packet_payload(encoded);
  for (std::size_t i = 0; i < repair_count; ++i) {
    append_packet_payload(redundancy_history[redundancy_history.size() - 1U - i]);
  }

  std::vector<std::byte> packet(udp_audio::protocol::kHeaderSizeBytes + payload.size());

  const PacketHeader header{
    .sequence = encoded.sequence,
    .timestamp_samples = encoded.timestamp_samples,
    .payload_size = static_cast<std::uint16_t>(payload.size()),
  };

  const auto header_bytes = udp_audio::protocol::serialize_header(header);
  std::memcpy(packet.data(), header_bytes.data(), header_bytes.size());
  std::memcpy(packet.data() + header_bytes.size(), payload.data(), payload.size());

  return packet;
}

bool encode_frame(OpusEncoder* encoder,
                  const MonoAudioFrame& frame,
                  EncodedPacket& encoded,
                  LoopbackStats& stats) {
  const auto byte_count = opus_encode_float(
    encoder, frame.samples.data(), static_cast<int>(udp_audio::audio::kFrameSamples),
    encoded.payload.data(), static_cast<opus_int32>(encoded.payload.size()));

  if (byte_count < 0) {
    std::cerr << "Opus encode failed: " << opus_strerror(byte_count) << '\n';
    return false;
  }

  encoded.payload_size = static_cast<std::size_t>(byte_count);
  encoded.sequence = frame.sequence;
  encoded.timestamp_samples = frame.timestamp_samples;
  ++stats.encoded;
  stats.opus_encoded_bytes += encoded.payload_size;
  return true;
}

void schedule_packet(std::vector<std::byte> packet,
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
    .bytes = std::move(packet),
    .deliver_at = Clock::now() + std::chrono::milliseconds(delay_ms),
  });
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

void enqueue_fadeout_frame(float last_sample, PlaybackState* playback_state) {
  if (playback_state == nullptr) {
    return;
  }

  auto fadeout_frame = udp_audio::audio::make_silent_frame(0);
  for (std::size_t i = 0; i < kPlaybackFadeOutSamples; ++i) {
    const auto fade =
      1.0F - (static_cast<float>(i) / static_cast<float>(kPlaybackFadeOutSamples - 1U));
    fadeout_frame.samples[i] = last_sample * fade;
  }

  if (playback_state->queue.push(fadeout_frame)) {
    playback_state->stats.fadeout_frames.fetch_add(1, std::memory_order_relaxed);
  } else {
    playback_state->stats.dropped_output_frames.fetch_add(1, std::memory_order_relaxed);
  }
}

std::optional<BufferedPacket> make_buffered_packet(const udp_audio::protocol::PacketHeader& header,
                                                   std::span<const std::byte> payload,
                                                   Clock::time_point arrival_time,
                                                   const std::vector<Clock::time_point>& sent_times,
                                                   LoopbackStats& stats) {
  constexpr std::uint8_t kBundleMagic = 0x4f;
  constexpr std::uint8_t kBundleVersion = 1;
  constexpr std::size_t kBundleHeaderBytes = 6;
  constexpr std::size_t kRepairDescriptorBytes = 10;

  if (payload.size() < kBundleHeaderBytes ||
      static_cast<std::uint8_t>(payload[0]) != kBundleMagic ||
      static_cast<std::uint8_t>(payload[1]) != kBundleVersion) {
    return std::nullopt;
  }

  const auto repair_count = static_cast<std::size_t>(payload[2]);
  if (repair_count > kMaxRedundantOpusFrames) {
    return std::nullopt;
  }

  const auto descriptor_bytes = repair_count * kRepairDescriptorBytes;
  const auto payload_header_bytes = kBundleHeaderBytes + descriptor_bytes;
  if (payload.size() < payload_header_bytes) {
    return std::nullopt;
  }

  const auto primary_size = static_cast<std::size_t>(read_u16(payload, 4));
  if (primary_size == 0 || primary_size > kMaxOpusPacketBytes ||
      payload.size() < payload_header_bytes + primary_size) {
    return std::nullopt;
  }

  double inter_arrival_ms = 0.0;
  if (stats.has_previous_arrival) {
    inter_arrival_ms =
      std::chrono::duration<double, std::milli>(arrival_time - stats.previous_arrival).count();
    stats.inter_arrival_sum_ms += inter_arrival_ms;
    stats.inter_arrival_max_ms = std::max(stats.inter_arrival_max_ms, inter_arrival_ms);
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

  BufferedPacket buffered{};
  buffered.packet.sequence = header.sequence;
  buffered.packet.timestamp_samples = header.timestamp_samples;
  buffered.packet.payload_size = primary_size;
  buffered.arrival_time = arrival_time;
  buffered.network_latency_ms = latency_ms;
  buffered.inter_arrival_ms = inter_arrival_ms;
  std::memcpy(buffered.packet.payload.data(), payload.data() + payload_header_bytes,
              primary_size);

  auto blob_offset = payload_header_bytes + primary_size;
  for (std::size_t i = 0; i < repair_count; ++i) {
    const auto descriptor_offset = kBundleHeaderBytes + (i * kRepairDescriptorBytes);
    auto& repair = buffered.redundant_packets[i];
    repair.sequence = read_u32(payload, descriptor_offset);
    repair.timestamp_samples = read_u32(payload, descriptor_offset + 4U);
    repair.payload_size = read_u16(payload, descriptor_offset + 8U);
    if (repair.payload_size == 0 || repair.payload_size > kMaxOpusPacketBytes ||
        payload.size() < blob_offset + repair.payload_size) {
      return std::nullopt;
    }
    std::memcpy(repair.payload.data(), payload.data() + blob_offset, repair.payload_size);
    blob_offset += repair.payload_size;
    ++buffered.redundant_packet_count;
  }

  return buffered;
}

bool drain_receiver(udp_audio::transport::UdpSocket& receiver,
                    std::vector<Clock::time_point>& sent_times,
                    udp_audio::jitter::FixedJitterBuffer<BufferedPacket, kJitterCapacityFrames>&
                      jitter_buffer,
                    LoopbackStats& stats,
                    std::error_code& error) {
  std::array<std::byte, udp_audio::protocol::kHeaderSizeBytes +
                         udp_audio::protocol::kMaxPayloadBytes>
    buffer{};

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
        result->bytes_received < udp_audio::protocol::kHeaderSizeBytes + header.payload_size) {
      ++stats.invalid_datagrams;
      continue;
    }

    const auto payload = std::span<const std::byte>(
      buffer.data() + udp_audio::protocol::kHeaderSizeBytes, header.payload_size);
    const auto buffered =
      make_buffered_packet(header, payload, Clock::now(), sent_times, stats);
    if (!buffered.has_value()) {
      ++stats.invalid_datagrams;
      continue;
    }
    static_cast<void>(jitter_buffer.push(header.sequence, *buffered));
  }
}

bool decode_encoded_opus_packet(OpusDecoder* decoder,
                                const EncodedPacket& packet,
                                MonoAudioFrame& output,
                                LoopbackStats& stats) {
  const auto decoded_samples = opus_decode_float(
    decoder, packet.payload.data(), static_cast<opus_int32>(packet.payload_size),
    output.samples.data(), static_cast<int>(udp_audio::audio::kFrameSamples), 0);

  if (decoded_samples != static_cast<int>(udp_audio::audio::kFrameSamples)) {
    ++stats.opus_decode_errors;
    std::fill(output.samples.begin(), output.samples.end(), 0.0F);
    return false;
  }

  output.sequence = packet.sequence;
  output.timestamp_samples = packet.timestamp_samples;
  ++stats.decoded;
  return true;
}

bool decode_opus_packet(OpusDecoder* decoder,
                        const BufferedPacket& packet,
                        MonoAudioFrame& output,
                        LoopbackStats& stats) {
  return decode_encoded_opus_packet(decoder, packet.packet, output, stats);
}

bool decode_opus_plc(OpusDecoder* decoder,
                     std::uint32_t sequence,
                     MonoAudioFrame& output,
                     LoopbackStats& stats) {
  const auto decoded_samples = opus_decode_float(
    decoder, nullptr, 0, output.samples.data(),
    static_cast<int>(udp_audio::audio::kFrameSamples), 0);

  output.sequence = sequence;
  output.timestamp_samples =
    sequence * static_cast<std::uint32_t>(udp_audio::audio::kFrameSamples);

  if (decoded_samples != static_cast<int>(udp_audio::audio::kFrameSamples)) {
    ++stats.opus_decode_errors;
    std::fill(output.samples.begin(), output.samples.end(), 0.0F);
    return false;
  }

  ++stats.concealed;
  ++stats.plc_generated;
  return true;
}

bool decode_opus_fec(OpusDecoder* decoder,
                     const BufferedPacket& repair_packet,
                     std::uint32_t missing_sequence,
                     MonoAudioFrame& output,
                     LoopbackStats& stats) {
  const auto decoded_samples = opus_decode_float(
    decoder, repair_packet.packet.payload.data(),
    static_cast<opus_int32>(repair_packet.packet.payload_size), output.samples.data(),
    static_cast<int>(udp_audio::audio::kFrameSamples), 1);

  output.sequence = missing_sequence;
  output.timestamp_samples =
    missing_sequence * static_cast<std::uint32_t>(udp_audio::audio::kFrameSamples);

  if (decoded_samples != static_cast<int>(udp_audio::audio::kFrameSamples)) {
    ++stats.opus_decode_errors;
    std::fill(output.samples.begin(), output.samples.end(), 0.0F);
    return false;
  }

  ++stats.concealed;
  ++stats.fec_recovered;
  return true;
}

std::optional<EncodedPacket> find_redundant_packet(
  const udp_audio::jitter::FixedJitterBuffer<BufferedPacket, kJitterCapacityFrames>& jitter_buffer,
  std::uint32_t sequence,
  std::size_t redundancy_frames) {
  for (std::size_t lookahead = 1; lookahead <= redundancy_frames; ++lookahead) {
    const auto carrier = jitter_buffer.peek_expected(
      sequence + static_cast<std::uint32_t>(lookahead));
    if (!carrier.has_value()) {
      continue;
    }

    for (std::size_t i = 0; i < carrier->redundant_packet_count; ++i) {
      if (carrier->redundant_packets[i].sequence == sequence) {
        return carrier->redundant_packets[i];
      }
    }
  }

  return std::nullopt;
}

void play_expected_frame(std::uint32_t sequence,
                         OpusDecoder* decoder,
                         udp_audio::jitter::FixedJitterBuffer<BufferedPacket,
                         kJitterCapacityFrames>& jitter_buffer,
                         bool use_fec,
                         std::size_t redundancy_frames,
                         std::size_t jitter_depth_frames,
                         PlaybackState* playback_state,
                         WavWriter* recorder,
                         float& last_output_sample,
                         const std::vector<Clock::time_point>& sent_times,
                         LoopbackStats& stats) {
  const auto now = Clock::now();
  const double playout_latency_ms =
    sequence < sent_times.size()
      ? std::chrono::duration<double, std::milli>(now - sent_times[sequence]).count()
      : 0.0;

  stats.playout_latency_sum_ms += playout_latency_ms;
  stats.playout_latency_max_ms = std::max(stats.playout_latency_max_ms, playout_latency_ms);

  auto packet = jitter_buffer.pop_expected(sequence);
  MonoAudioFrame output_frame{};
  if (!packet.has_value()) {
    bool recovered_with_redundancy = false;
    bool recovered_with_fec = false;
    const auto redundant_packet = find_redundant_packet(jitter_buffer, sequence, redundancy_frames);
    if (redundant_packet.has_value()) {
      recovered_with_redundancy =
        decode_encoded_opus_packet(decoder, *redundant_packet, output_frame, stats);
      if (recovered_with_redundancy) {
        ++stats.redundancy_recovered;
      }
    }
    if (use_fec) {
      const auto repair_packet = jitter_buffer.peek_expected(sequence + 1U);
      if (!recovered_with_redundancy && repair_packet.has_value()) {
        recovered_with_fec = decode_opus_fec(decoder, *repair_packet, sequence, output_frame, stats);
      }
    }
    if (!recovered_with_redundancy && !recovered_with_fec) {
      static_cast<void>(decode_opus_plc(decoder, sequence, output_frame, stats));
    }
    if (recorder != nullptr) {
      recorder->write_frame(output_frame);
    }
    enqueue_for_playback(output_frame, playback_state);
    last_output_sample = output_frame.samples.back();
    std::cout << sequence << ',' << output_frame.timestamp_samples << ",,"
              << playout_latency_ms << ",," << jitter_depth_frames << ','
              << (recovered_with_redundancy
                    ? "opus_redundant"
                    : (recovered_with_fec ? "opus_fec" : "opus_plc"))
              << '\n';
    return;
  }

  static_cast<void>(decode_opus_packet(decoder, *packet, output_frame, stats));
  if (recorder != nullptr) {
    recorder->write_frame(output_frame);
  }
  enqueue_for_playback(output_frame, playback_state);
  last_output_sample = output_frame.samples.back();

  std::cout << sequence << ',' << output_frame.timestamp_samples << ','
            << packet->network_latency_ms << ',' << playout_latency_ms << ','
            << packet->inter_arrival_ms << ',' << jitter_depth_frames << ",decoded\n";
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  const auto frame_count = options.frame_count;

  int opus_error = OPUS_OK;
  OpusEncoder* encoder =
    opus_encoder_create(udp_audio::audio::kSampleRateHz, udp_audio::audio::kChannels,
                        OPUS_APPLICATION_AUDIO, &opus_error);
  if (opus_error != OPUS_OK || encoder == nullptr) {
    std::cerr << "Failed to create Opus encoder: " << opus_strerror(opus_error) << '\n';
    return 1;
  }

  OpusDecoder* decoder =
    opus_decoder_create(udp_audio::audio::kSampleRateHz, udp_audio::audio::kChannels,
                        &opus_error);
  if (opus_error != OPUS_OK || decoder == nullptr) {
    std::cerr << "Failed to create Opus decoder: " << opus_strerror(opus_error) << '\n';
    opus_encoder_destroy(encoder);
    return 1;
  }

  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(options.bitrate_bps));
  opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
  const bool use_fec = options.recovery_mode == "fec";
  opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(static_cast<opus_int32>(options.loss_percent)));
  if (use_fec) {
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
  }

  std::error_code error;
  auto receiver = udp_audio::transport::UdpSocket::open_ipv4(error);
  if (error || !receiver.valid()) {
    std::cerr << "Failed to open receiver socket: " << error.message() << '\n';
    opus_decoder_destroy(decoder);
    opus_encoder_destroy(encoder);
    return 1;
  }

  if (!receiver.bind(udp_audio::transport::Endpoint::loopback(0), error)) {
    std::cerr << "Failed to bind receiver socket: " << error.message() << '\n';
    opus_decoder_destroy(decoder);
    opus_encoder_destroy(encoder);
    return 1;
  }

  const auto bound_endpoint = receiver.local_endpoint(error);
  if (error || !bound_endpoint.has_value()) {
    std::cerr << "Failed to inspect receiver endpoint: " << error.message() << '\n';
    opus_decoder_destroy(decoder);
    opus_encoder_destroy(encoder);
    return 1;
  }
  const auto receiver_endpoint = udp_audio::transport::Endpoint::loopback(bound_endpoint->port);

  auto sender = udp_audio::transport::UdpSocket::open_ipv4(error);
  if (error || !sender.valid()) {
    std::cerr << "Failed to open sender socket: " << error.message() << '\n';
    opus_decoder_destroy(decoder);
    opus_encoder_destroy(encoder);
    return 1;
  }

  WavWriter recorder{};
  WavWriter* active_recorder = nullptr;
  if (!options.record_wav_path.empty()) {
    if (!recorder.open(options.record_wav_path)) {
      std::cerr << "Failed to open recording WAV: " << options.record_wav_path << '\n';
      opus_decoder_destroy(decoder);
      opus_encoder_destroy(encoder);
      return 1;
    }
    active_recorder = &recorder;
  }

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
      recorder.close();
      opus_decoder_destroy(decoder);
      opus_encoder_destroy(encoder);
      return 1;
    }
    playback_device_initialized = true;
    if (ma_device_start(&playback_device) != MA_SUCCESS) {
      std::cerr << "Failed to start playback device\n";
      ma_device_uninit(&playback_device);
      recorder.close();
      opus_decoder_destroy(decoder);
      opus_encoder_destroy(encoder);
      return 1;
    }
    playback_started = true;
  }

  const auto cleanup = [&]() {
    recorder.close();
    if (playback_started) {
      ma_device_stop(&playback_device);
    }
    if (playback_device_initialized) {
      ma_device_uninit(&playback_device);
    }
    opus_decoder_destroy(decoder);
    opus_encoder_destroy(encoder);
  };

  const auto source_mode = parse_source_mode(options.source_mode);
  udp_audio::jitter::FixedJitterBuffer<BufferedPacket, kJitterCapacityFrames> jitter_buffer(
    options.jitter_depth_frames);
  LoopbackStats stats{};
  std::vector<Clock::time_point> sent_times(frame_count);
  std::deque<PendingPacket> pending_packets;
  std::deque<EncodedPacket> redundancy_history;
  std::mt19937 rng(options.seed);
  double phase = 0.0;
  const auto stream_start = Clock::now();
  auto next_send_time = stream_start;
  float last_output_sample = 0.0F;

  std::cout << "sequence,timestamp_samples,network_latency_ms,playout_latency_ms,"
               "inter_arrival_ms,jitter_depth_frames,status\n";

  for (std::size_t i = 0; i < frame_count; ++i) {
    std::this_thread::sleep_until(next_send_time);

    const auto frame = make_source_frame(source_mode, static_cast<std::uint32_t>(i),
                                         frame_count, phase);
    EncodedPacket encoded{};
    if (!encode_frame(encoder, frame, encoded, stats)) {
      cleanup();
      return 1;
    }

    sent_times[i] = Clock::now();
    schedule_packet(make_packet(encoded, redundancy_history, options.redundancy_frames, stats),
                    options, rng, pending_packets, stats);
    redundancy_history.push_back(encoded);
    while (redundancy_history.size() > options.redundancy_frames) {
      redundancy_history.pop_front();
    }

    if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
      std::cerr << "Failed to send due packets: " << error.message() << '\n';
      cleanup();
      return 1;
    }

    const auto receive_until = Clock::now() + std::chrono::milliseconds(2);
    while (Clock::now() < receive_until) {
      if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
        std::cerr << "Failed to send due packets while receiving: " << error.message() << '\n';
        cleanup();
        return 1;
      }
      if (!drain_receiver(receiver, sent_times, jitter_buffer, stats, error)) {
        std::cerr << "Failed to receive packet: " << error.message() << '\n';
        cleanup();
        return 1;
      }
      if (pending_packets.empty() && stats.received >= stats.sent) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    if (i >= options.jitter_depth_frames) {
      play_expected_frame(static_cast<std::uint32_t>(i - options.jitter_depth_frames), decoder,
                          jitter_buffer, use_fec, options.redundancy_frames,
                          options.jitter_depth_frames, playback, active_recorder,
                          last_output_sample, sent_times, stats);
    }

    next_send_time += std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs);
  }

  const auto drain_until = Clock::now() + std::chrono::milliseconds(200 + options.jitter_ms);
  while (Clock::now() < drain_until && (!pending_packets.empty() || stats.received < stats.sent)) {
    if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
      std::cerr << "Failed to send delayed packets: " << error.message() << '\n';
      cleanup();
      return 1;
    }
    if (!drain_receiver(receiver, sent_times, jitter_buffer, stats, error)) {
      std::cerr << "Failed to drain receiver: " << error.message() << '\n';
      cleanup();
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  auto tail_play_time =
    stream_start + std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs *
                                             static_cast<std::uint32_t>(options.jitter_depth_frames));
  if (frame_count > options.jitter_depth_frames) {
    tail_play_time = next_send_time;
  }

  for (std::size_t sequence =
         frame_count > options.jitter_depth_frames ? frame_count - options.jitter_depth_frames : 0;
       sequence < frame_count; ++sequence) {
    std::this_thread::sleep_until(tail_play_time);
    if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
      std::cerr << "Failed to send delayed packets before tail playout: " << error.message()
                << '\n';
      cleanup();
      return 1;
    }
    if (!drain_receiver(receiver, sent_times, jitter_buffer, stats, error)) {
      std::cerr << "Failed to drain receiver before tail playout: " << error.message() << '\n';
      cleanup();
      return 1;
    }
    play_expected_frame(static_cast<std::uint32_t>(sequence), decoder, jitter_buffer,
                        use_fec, options.redundancy_frames, options.jitter_depth_frames,
                        playback, active_recorder, last_output_sample, sent_times, stats);
    tail_play_time += std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs);
  }

  if (playback != nullptr) {
    for (std::size_t i = 0; i < kPlaybackFadeOutFrames; ++i) {
      enqueue_fadeout_frame(last_output_sample, playback);
    }
  }

  if (playback_started) {
    const auto expected_device_frames =
      (stats.decoded + stats.concealed + kPlaybackPrerollFrames + kPlaybackFadeOutFrames) *
      udp_audio::audio::kFrameSamples;
    const auto playback_deadline =
      Clock::now() + std::chrono::milliseconds(
                       250 + 10 * static_cast<int>(options.jitter_depth_frames));
    while (Clock::now() < playback_deadline &&
           playback_state.stats.rendered_device_frames.load(std::memory_order_relaxed) <
             expected_device_frames) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ma_device_stop(&playback_device);
    playback_started = false;
  }

  if (playback_device_initialized) {
    ma_device_uninit(&playback_device);
    playback_device_initialized = false;
  }

  recorder.close();
  opus_decoder_destroy(decoder);
  opus_encoder_destroy(encoder);

  const auto output_frames = stats.decoded + stats.concealed;
  const auto average_latency =
    stats.received == 0 ? 0.0 : stats.latency_sum_ms / static_cast<double>(stats.received);
  const auto average_playout_latency =
    output_frames == 0 ? 0.0 : stats.playout_latency_sum_ms / static_cast<double>(output_frames);
  const auto inter_arrival_samples = stats.received > 1 ? stats.received - 1U : 0U;
  const auto average_inter_arrival =
    inter_arrival_samples == 0
      ? 0.0
      : stats.inter_arrival_sum_ms / static_cast<double>(inter_arrival_samples);
  const auto average_packet_bytes =
    stats.encoded == 0 ? 0.0 : static_cast<double>(stats.opus_encoded_bytes) /
                                   static_cast<double>(stats.encoded);
  const auto average_redundancy_bytes =
    stats.encoded == 0 ? 0.0 : static_cast<double>(stats.redundancy_bytes) /
                                   static_cast<double>(stats.encoded);

  std::cout << "\nsummary\n";
  std::cout << "receiver=" << receiver_endpoint.address << ':' << receiver_endpoint.port << '\n';
  std::cout << "codec=opus\n";
  std::cout << "opus_application=audio\n";
  std::cout << "opus_signal=music\n";
  std::cout << "opus_bitrate_bps=" << options.bitrate_bps << '\n';
  std::cout << "opus_frame_samples=" << udp_audio::audio::kFrameSamples << '\n';
  std::cout << "opus_recovery_mode=" << options.recovery_mode << '\n';
  std::cout << "opus_inband_fec=" << (use_fec ? 1 : 0) << '\n';
  std::cout << "opus_redundancy_frames=" << options.redundancy_frames << '\n';
  std::cout << "configured_loss_percent=" << options.loss_percent << '\n';
  std::cout << "configured_jitter_ms=" << options.jitter_ms << '\n';
  std::cout << "seed=" << options.seed << '\n';
  std::cout << "play_audio=" << (options.play_audio ? 1 : 0) << '\n';
  std::cout << "record_wav="
            << (options.record_wav_path.empty() ? "(none)" : options.record_wav_path) << '\n';
  std::cout << "source_mode=" << options.source_mode << '\n';
  std::cout << "generated=" << stats.generated << '\n';
  std::cout << "encoded=" << stats.encoded << '\n';
  std::cout << "dropped_by_impairment=" << stats.dropped << '\n';
  std::cout << "sent=" << stats.sent << '\n';
  std::cout << "received=" << stats.received << '\n';
  std::cout << "decoded=" << stats.decoded << '\n';
  std::cout << "concealed=" << stats.concealed << '\n';
  std::cout << "redundancy_recovered=" << stats.redundancy_recovered << '\n';
  std::cout << "fec_decode_attempts=" << stats.fec_recovered << '\n';
  std::cout << "plc_generated=" << stats.plc_generated << '\n';
  std::cout << "output_frames=" << output_frames << '\n';
  std::cout << "datagrams=" << stats.datagrams << '\n';
  std::cout << "invalid_datagrams=" << stats.invalid_datagrams << '\n';
  std::cout << "jitter_depth_frames=" << jitter_buffer.stats().target_depth_frames << '\n';
  std::cout << "jitter_underruns=" << jitter_buffer.stats().underruns << '\n';
  std::cout << "missing_frames=" << jitter_buffer.stats().underruns << '\n';
  std::cout << "opus_fec_decode_attempts=" << stats.fec_recovered << '\n';
  std::cout << "opus_plc_frames=" << stats.plc_generated << '\n';
  std::cout << "opus_decode_errors=" << stats.opus_decode_errors << '\n';
  std::cout << "avg_opus_packet_bytes=" << average_packet_bytes << '\n';
  std::cout << "avg_redundancy_bytes=" << average_redundancy_bytes << '\n';
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
