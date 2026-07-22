#include "udp_audio/audio/frame.hpp"
#include "udp_audio/jitter/fixed_jitter_buffer.hpp"
#include "udp_audio/protocol/packet.hpp"
#include "udp_audio/transport/udp_socket.hpp"

#include <opus/opus.h>

#include <algorithm>
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
constexpr std::size_t kJitterDepthFrames = 3;
constexpr std::size_t kJitterCapacityFrames = 64;
constexpr std::size_t kMaxOpusPacketBytes = 1500;
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
};

struct EncodedPacket {
  std::array<unsigned char, kMaxOpusPacketBytes> payload{};
  std::size_t payload_size = 0;
  std::uint32_t sequence = 0;
  std::uint32_t timestamp_samples = 0;
};

struct BufferedPacket {
  EncodedPacket packet{};
  Clock::time_point arrival_time{};
  double network_latency_ms = 0.0;
  double inter_arrival_ms = 0.0;
};

struct PendingPacket {
  std::vector<std::byte> bytes{};
  Clock::time_point deliver_at{};
};

struct LoopbackStats {
  std::size_t generated = 0;
  std::size_t encoded = 0;
  std::size_t dropped = 0;
  std::size_t sent = 0;
  std::size_t received = 0;
  std::size_t decoded = 0;
  std::size_t concealed = 0;
  std::size_t datagrams = 0;
  std::size_t invalid_datagrams = 0;
  std::size_t opus_decode_errors = 0;
  std::size_t opus_encoded_bytes = 0;
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

  if (argc > 1 && !parse_unsigned_arg(std::string_view(argv[1]), options.frame_count)) {
    std::cerr << "Usage: udp_audio_opus_loopback [frame_count] [loss_percent] [jitter_ms] "
                 "[seed] [record_wav] [source_mode] [bitrate_bps]\n";
    options.frame_count = kDefaultFrameCount;
  }

  if (argc > 2 && !parse_unsigned_arg(std::string_view(argv[2]), options.loss_percent)) {
    std::cerr << "Usage: udp_audio_opus_loopback [frame_count] [loss_percent] [jitter_ms] "
                 "[seed] [record_wav] [source_mode] [bitrate_bps]\n";
    options.loss_percent = 0;
  }

  if (argc > 3 && !parse_unsigned_arg(std::string_view(argv[3]), options.jitter_ms)) {
    std::cerr << "Usage: udp_audio_opus_loopback [frame_count] [loss_percent] [jitter_ms] "
                 "[seed] [record_wav] [source_mode] [bitrate_bps]\n";
    options.jitter_ms = 0;
  }

  if (argc > 4 && !parse_unsigned_arg(std::string_view(argv[4]), options.seed)) {
    std::cerr << "Usage: udp_audio_opus_loopback [frame_count] [loss_percent] [jitter_ms] "
                 "[seed] [record_wav] [source_mode] [bitrate_bps]\n";
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
    std::cerr << "Usage: udp_audio_opus_loopback [frame_count] [loss_percent] [jitter_ms] "
                 "[seed] [record_wav] [source_mode] [bitrate_bps]\n";
    options.bitrate_bps = kDefaultBitrateBps;
  }

  if (options.frame_count == 0) {
    options.frame_count = kDefaultFrameCount;
  }
  options.loss_percent = std::min<std::uint32_t>(options.loss_percent, 100);
  options.bitrate_bps = std::max<std::int32_t>(6000, options.bitrate_bps);
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

std::vector<std::byte> make_packet(const EncodedPacket& encoded) {
  using udp_audio::protocol::PacketHeader;

  std::vector<std::byte> packet(udp_audio::protocol::kHeaderSizeBytes + encoded.payload_size);

  const PacketHeader header{
    .sequence = encoded.sequence,
    .timestamp_samples = encoded.timestamp_samples,
    .payload_size = static_cast<std::uint16_t>(encoded.payload_size),
  };

  const auto header_bytes = udp_audio::protocol::serialize_header(header);
  std::memcpy(packet.data(), header_bytes.data(), header_bytes.size());
  std::memcpy(packet.data() + header_bytes.size(), encoded.payload.data(), encoded.payload_size);

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

BufferedPacket make_buffered_packet(const udp_audio::protocol::PacketHeader& header,
                                    std::span<const std::byte> payload,
                                    Clock::time_point arrival_time,
                                    const std::vector<Clock::time_point>& sent_times,
                                    LoopbackStats& stats) {
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
  buffered.packet.payload_size = header.payload_size;
  buffered.arrival_time = arrival_time;
  buffered.network_latency_ms = latency_ms;
  buffered.inter_arrival_ms = inter_arrival_ms;
  std::memcpy(buffered.packet.payload.data(), payload.data(), header.payload_size);
  return buffered;
}

bool drain_receiver(udp_audio::transport::UdpSocket& receiver,
                    std::vector<Clock::time_point>& sent_times,
                    udp_audio::jitter::FixedJitterBuffer<BufferedPacket, kJitterCapacityFrames>&
                      jitter_buffer,
                    LoopbackStats& stats,
                    std::error_code& error) {
  std::array<std::byte, udp_audio::protocol::kHeaderSizeBytes + kMaxOpusPacketBytes> buffer{};

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
        header.payload_size > kMaxOpusPacketBytes ||
        result->bytes_received < udp_audio::protocol::kHeaderSizeBytes + header.payload_size) {
      ++stats.invalid_datagrams;
      continue;
    }

    const auto payload = std::span<const std::byte>(
      buffer.data() + udp_audio::protocol::kHeaderSizeBytes, header.payload_size);
    const auto buffered =
      make_buffered_packet(header, payload, Clock::now(), sent_times, stats);
    static_cast<void>(jitter_buffer.push(header.sequence, buffered));
  }
}

bool decode_opus_packet(OpusDecoder* decoder,
                        const BufferedPacket& packet,
                        MonoAudioFrame& output,
                        LoopbackStats& stats) {
  const auto decoded_samples = opus_decode_float(
    decoder, packet.packet.payload.data(), static_cast<opus_int32>(packet.packet.payload_size),
    output.samples.data(), static_cast<int>(udp_audio::audio::kFrameSamples), 0);

  if (decoded_samples != static_cast<int>(udp_audio::audio::kFrameSamples)) {
    ++stats.opus_decode_errors;
    std::fill(output.samples.begin(), output.samples.end(), 0.0F);
    return false;
  }

  output.sequence = packet.packet.sequence;
  output.timestamp_samples = packet.packet.timestamp_samples;
  ++stats.decoded;
  return true;
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
  return true;
}

void play_expected_frame(std::uint32_t sequence,
                         OpusDecoder* decoder,
                         udp_audio::jitter::FixedJitterBuffer<BufferedPacket,
                                                               kJitterCapacityFrames>& jitter_buffer,
                         WavWriter* recorder,
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
    static_cast<void>(decode_opus_plc(decoder, sequence, output_frame, stats));
    if (recorder != nullptr) {
      recorder->write_frame(output_frame);
    }
    std::cout << sequence << ',' << output_frame.timestamp_samples << ",,"
              << playout_latency_ms << ",," << kJitterDepthFrames << ",opus_plc\n";
    return;
  }

  static_cast<void>(decode_opus_packet(decoder, *packet, output_frame, stats));
  if (recorder != nullptr) {
    recorder->write_frame(output_frame);
  }

  std::cout << sequence << ',' << output_frame.timestamp_samples << ','
            << packet->network_latency_ms << ',' << playout_latency_ms << ','
            << packet->inter_arrival_ms << ',' << kJitterDepthFrames << ",decoded\n";
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

  const auto source_mode = parse_source_mode(options.source_mode);
  udp_audio::jitter::FixedJitterBuffer<BufferedPacket, kJitterCapacityFrames> jitter_buffer(
    kJitterDepthFrames);
  LoopbackStats stats{};
  std::vector<Clock::time_point> sent_times(frame_count);
  std::deque<PendingPacket> pending_packets;
  std::mt19937 rng(options.seed);
  double phase = 0.0;
  const auto stream_start = Clock::now();
  auto next_send_time = stream_start;

  std::cout << "sequence,timestamp_samples,network_latency_ms,playout_latency_ms,"
               "inter_arrival_ms,jitter_depth_frames,status\n";

  for (std::size_t i = 0; i < frame_count; ++i) {
    std::this_thread::sleep_until(next_send_time);

    const auto frame = make_source_frame(source_mode, static_cast<std::uint32_t>(i),
                                         frame_count, phase);
    EncodedPacket encoded{};
    if (!encode_frame(encoder, frame, encoded, stats)) {
      recorder.close();
      opus_decoder_destroy(decoder);
      opus_encoder_destroy(encoder);
      return 1;
    }

    sent_times[i] = Clock::now();
    schedule_packet(make_packet(encoded), options, rng, pending_packets, stats);

    if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
      std::cerr << "Failed to send due packets: " << error.message() << '\n';
      recorder.close();
      opus_decoder_destroy(decoder);
      opus_encoder_destroy(encoder);
      return 1;
    }

    const auto receive_until = Clock::now() + std::chrono::milliseconds(2);
    while (Clock::now() < receive_until) {
      if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
        std::cerr << "Failed to send due packets while receiving: " << error.message() << '\n';
        recorder.close();
        opus_decoder_destroy(decoder);
        opus_encoder_destroy(encoder);
        return 1;
      }
      if (!drain_receiver(receiver, sent_times, jitter_buffer, stats, error)) {
        std::cerr << "Failed to receive packet: " << error.message() << '\n';
        recorder.close();
        opus_decoder_destroy(decoder);
        opus_encoder_destroy(encoder);
        return 1;
      }
      if (pending_packets.empty() && stats.received >= stats.sent) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    if (i >= kJitterDepthFrames) {
      play_expected_frame(static_cast<std::uint32_t>(i - kJitterDepthFrames), decoder,
                          jitter_buffer, active_recorder, sent_times, stats);
    }

    next_send_time += std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs);
  }

  const auto drain_until = Clock::now() + std::chrono::milliseconds(200 + options.jitter_ms);
  while (Clock::now() < drain_until && (!pending_packets.empty() || stats.received < stats.sent)) {
    if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
      std::cerr << "Failed to send delayed packets: " << error.message() << '\n';
      recorder.close();
      opus_decoder_destroy(decoder);
      opus_encoder_destroy(encoder);
      return 1;
    }
    if (!drain_receiver(receiver, sent_times, jitter_buffer, stats, error)) {
      std::cerr << "Failed to drain receiver: " << error.message() << '\n';
      recorder.close();
      opus_decoder_destroy(decoder);
      opus_encoder_destroy(encoder);
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  auto tail_play_time =
    stream_start + std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs *
                                             static_cast<std::uint32_t>(kJitterDepthFrames));
  if (frame_count > kJitterDepthFrames) {
    tail_play_time = next_send_time;
  }

  for (std::size_t sequence =
         frame_count > kJitterDepthFrames ? frame_count - kJitterDepthFrames : 0;
       sequence < frame_count; ++sequence) {
    std::this_thread::sleep_until(tail_play_time);
    if (!release_due_packets(pending_packets, sender, receiver_endpoint, stats, error)) {
      std::cerr << "Failed to send delayed packets before tail playout: " << error.message()
                << '\n';
      recorder.close();
      opus_decoder_destroy(decoder);
      opus_encoder_destroy(encoder);
      return 1;
    }
    if (!drain_receiver(receiver, sent_times, jitter_buffer, stats, error)) {
      std::cerr << "Failed to drain receiver before tail playout: " << error.message() << '\n';
      recorder.close();
      opus_decoder_destroy(decoder);
      opus_encoder_destroy(encoder);
      return 1;
    }
    play_expected_frame(static_cast<std::uint32_t>(sequence), decoder, jitter_buffer,
                        active_recorder, sent_times, stats);
    tail_play_time += std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs);
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

  std::cout << "\nsummary\n";
  std::cout << "receiver=" << receiver_endpoint.address << ':' << receiver_endpoint.port << '\n';
  std::cout << "codec=opus\n";
  std::cout << "opus_application=audio\n";
  std::cout << "opus_signal=music\n";
  std::cout << "opus_bitrate_bps=" << options.bitrate_bps << '\n';
  std::cout << "opus_frame_samples=" << udp_audio::audio::kFrameSamples << '\n';
  std::cout << "configured_loss_percent=" << options.loss_percent << '\n';
  std::cout << "configured_jitter_ms=" << options.jitter_ms << '\n';
  std::cout << "seed=" << options.seed << '\n';
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
  std::cout << "output_frames=" << output_frames << '\n';
  std::cout << "datagrams=" << stats.datagrams << '\n';
  std::cout << "invalid_datagrams=" << stats.invalid_datagrams << '\n';
  std::cout << "jitter_depth_frames=" << jitter_buffer.stats().target_depth_frames << '\n';
  std::cout << "jitter_underruns=" << jitter_buffer.stats().underruns << '\n';
  std::cout << "missing_frames=" << jitter_buffer.stats().underruns << '\n';
  std::cout << "opus_plc_frames=" << stats.concealed << '\n';
  std::cout << "opus_decode_errors=" << stats.opus_decode_errors << '\n';
  std::cout << "avg_opus_packet_bytes=" << average_packet_bytes << '\n';
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
}
