#include "udp_audio/audio/frame.hpp"
#include "udp_audio/jitter/fixed_jitter_buffer.hpp"
#include "udp_audio/protocol/packet.hpp"
#include "udp_audio/transport/udp_socket.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using udp_audio::audio::MonoAudioFrame;

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 440.0;
constexpr float kToneGain = 0.2F;
constexpr std::size_t kDefaultFrameCount = 100;
constexpr std::size_t kJitterDepthFrames = 3;
constexpr std::size_t kJitterCapacityFrames = 64;

struct BufferedFrame {
  MonoAudioFrame frame{};
  Clock::time_point arrival_time{};
  double network_latency_ms = 0.0;
  double inter_arrival_ms = 0.0;
};

struct LoopbackStats {
  std::size_t sent = 0;
  std::size_t received = 0;
  std::size_t played = 0;
  std::size_t datagrams = 0;
  std::size_t invalid_datagrams = 0;
  std::uint32_t missing_packets = 0;
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
};

std::size_t parse_frame_count(int argc, char** argv) {
  if (argc < 2) {
    return kDefaultFrameCount;
  }

  std::size_t value = 0;
  const std::string_view input(argv[1]);
  const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
  if (result.ec != std::errc{} || value == 0) {
    std::cerr << "Usage: udp_audio_loopback [frame_count]\n";
    return kDefaultFrameCount;
  }

  return value;
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

std::array<std::byte, udp_audio::protocol::kHeaderSizeBytes + udp_audio::audio::kFramePayloadBytes>
make_packet(const MonoAudioFrame& frame) {
  using udp_audio::protocol::PacketHeader;

  std::array<std::byte, udp_audio::protocol::kHeaderSizeBytes +
                         udp_audio::audio::kFramePayloadBytes>
    packet{};

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
                                  LoopbackStats& stats) {
  if (stats.has_previous_sequence && header.sequence > stats.previous_sequence + 1U) {
    stats.missing_packets += header.sequence - stats.previous_sequence - 1U;
  }

  stats.previous_sequence = header.sequence;
  stats.has_previous_sequence = true;

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

    const auto payload = std::span<const std::byte>(
      buffer.data() + udp_audio::protocol::kHeaderSizeBytes, udp_audio::audio::kFramePayloadBytes);
    const auto buffered =
      make_buffered_frame(header, payload, Clock::now(), sent_times, stats);
    static_cast<void>(jitter_buffer.push(header.sequence, buffered));
  }
}

void play_expected_frame(std::uint32_t sequence,
                         udp_audio::jitter::FixedJitterBuffer<BufferedFrame,
                                                               kJitterCapacityFrames>& jitter_buffer,
                         const std::vector<Clock::time_point>& sent_times,
                         LoopbackStats& stats) {
  const auto now = Clock::now();
  auto frame = jitter_buffer.pop_expected(sequence);
  if (!frame.has_value()) {
    std::cout << sequence << ",,,," << kJitterDepthFrames << ",underrun\n";
    return;
  }

  const double playout_latency_ms =
    sequence < sent_times.size()
      ? std::chrono::duration<double, std::milli>(now - sent_times[sequence]).count()
      : 0.0;
  stats.playout_latency_sum_ms += playout_latency_ms;
  stats.playout_latency_max_ms = std::max(stats.playout_latency_max_ms, playout_latency_ms);
  ++stats.played;

  std::cout << sequence << ',' << frame->frame.timestamp_samples << ','
            << frame->network_latency_ms << ',' << playout_latency_ms << ','
            << frame->inter_arrival_ms << ',' << kJitterDepthFrames << ",played\n";
}

}  // namespace

int main(int argc, char** argv) {
  const auto frame_count = parse_frame_count(argc, argv);

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
  udp_audio::jitter::FixedJitterBuffer<BufferedFrame, kJitterCapacityFrames> jitter_buffer(
    kJitterDepthFrames);
  LoopbackStats stats{};
  double phase = 0.0;
  const auto stream_start = Clock::now();
  auto next_send_time = stream_start;

  std::cout << "sequence,timestamp_samples,network_latency_ms,playout_latency_ms,"
               "inter_arrival_ms,jitter_depth_frames,status\n";

  for (std::size_t i = 0; i < frame_count; ++i) {
    std::this_thread::sleep_until(next_send_time);

    const auto frame = make_sine_frame(static_cast<std::uint32_t>(i), phase);
    const auto packet = make_packet(frame);
    sent_times[i] = Clock::now();

    const auto sent = sender.send_to(packet, receiver_endpoint, error);
    if (error || sent != packet.size()) {
      std::cerr << "Failed to send packet " << i << ": " << error.message() << '\n';
      return 1;
    }

    ++stats.sent;

    const auto receive_until = Clock::now() + std::chrono::milliseconds(2);
    while (Clock::now() < receive_until) {
      if (!drain_receiver(receiver, sent_times, jitter_buffer, stats, error)) {
        std::cerr << "Failed to receive packet: " << error.message() << '\n';
        return 1;
      }
      if (stats.received >= stats.sent) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    if (i >= kJitterDepthFrames) {
      play_expected_frame(static_cast<std::uint32_t>(i - kJitterDepthFrames), jitter_buffer,
                          sent_times, stats);
    }

    next_send_time += std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs);
  }

  const auto drain_until = Clock::now() + std::chrono::milliseconds(200);
  while (Clock::now() < drain_until && stats.received < stats.sent) {
    if (!drain_receiver(receiver, sent_times, jitter_buffer, stats, error)) {
      std::cerr << "Failed to drain receiver: " << error.message() << '\n';
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
    if (!drain_receiver(receiver, sent_times, jitter_buffer, stats, error)) {
      std::cerr << "Failed to drain receiver before tail playout: " << error.message() << '\n';
      return 1;
    }
    play_expected_frame(static_cast<std::uint32_t>(sequence), jitter_buffer, sent_times, stats);
    tail_play_time += std::chrono::milliseconds(udp_audio::audio::kFrameDurationMs);
  }

  const auto average_latency =
    stats.received == 0 ? 0.0 : stats.latency_sum_ms / static_cast<double>(stats.received);
  const auto average_playout_latency =
    stats.played == 0 ? 0.0 : stats.playout_latency_sum_ms / static_cast<double>(stats.played);
  const auto inter_arrival_samples = stats.received > 1 ? stats.received - 1U : 0U;
  const auto average_inter_arrival =
    inter_arrival_samples == 0
      ? 0.0
      : stats.inter_arrival_sum_ms / static_cast<double>(inter_arrival_samples);

  std::cout << "\nsummary\n";
  std::cout << "receiver=" << receiver_endpoint.address << ':' << receiver_endpoint.port << '\n';
  std::cout << "sent=" << stats.sent << '\n';
  std::cout << "received=" << stats.received << '\n';
  std::cout << "played=" << stats.played << '\n';
  std::cout << "datagrams=" << stats.datagrams << '\n';
  std::cout << "invalid_datagrams=" << stats.invalid_datagrams << '\n';
  std::cout << "jitter_depth_frames=" << jitter_buffer.stats().target_depth_frames << '\n';
  std::cout << "jitter_underruns=" << jitter_buffer.stats().underruns << '\n';
  std::cout << "missing_packets=" << stats.missing_packets + (stats.sent - stats.received) << '\n';
  std::cout << "avg_network_latency_ms=" << average_latency << '\n';
  std::cout << "min_network_latency_ms=" << (stats.received == 0 ? 0.0 : stats.latency_min_ms)
            << '\n';
  std::cout << "max_network_latency_ms=" << stats.latency_max_ms << '\n';
  std::cout << "avg_playout_latency_ms=" << average_playout_latency << '\n';
  std::cout << "max_playout_latency_ms=" << stats.playout_latency_max_ms << '\n';
  std::cout << "avg_inter_arrival_ms=" << average_inter_arrival << '\n';
  std::cout << "max_inter_arrival_ms=" << stats.inter_arrival_max_ms << '\n';
}
