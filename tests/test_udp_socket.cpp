#include "udp_audio/audio/frame.hpp"
#include "udp_audio/protocol/packet.hpp"
#include "udp_audio/transport/udp_socket.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <span>
#include <system_error>
#include <thread>

namespace {

void udp_socket_sends_audio_packet_on_loopback() {
  std::error_code error;
  auto receiver = udp_audio::transport::UdpSocket::open_ipv4(error);
  assert(!error);
  assert(receiver.valid());
  if (!receiver.bind(udp_audio::transport::Endpoint::loopback(0), error)) {
    if (error == std::errc::operation_not_permitted) {
      std::cerr << "skipping UDP loopback test: socket bind is not permitted here\n";
      return;
    }

    std::cerr << "receiver bind failed: " << error.message() << '\n';
    assert(false);
  }

  const auto receiver_endpoint = receiver.local_endpoint(error);
  assert(!error);
  assert(receiver_endpoint.has_value());

  auto sender = udp_audio::transport::UdpSocket::open_ipv4(error);
  assert(!error);
  assert(sender.valid());

  const auto frame = udp_audio::audio::make_silent_frame(9);
  const udp_audio::protocol::PacketHeader header{
    .sequence = frame.sequence,
    .timestamp_samples = frame.timestamp_samples,
    .payload_size = static_cast<std::uint16_t>(udp_audio::audio::kFramePayloadBytes),
  };

  std::array<std::byte, udp_audio::protocol::kHeaderSizeBytes +
                         udp_audio::audio::kFramePayloadBytes>
    packet{};
  const auto header_bytes = udp_audio::protocol::serialize_header(header);
  std::memcpy(packet.data(), header_bytes.data(), header_bytes.size());
  std::memcpy(packet.data() + header_bytes.size(), frame.samples.data(),
              udp_audio::audio::kFramePayloadBytes);

  const auto sent = sender.send_to(packet, *receiver_endpoint, error);
  assert(!error);
  assert(sent == packet.size());

  std::array<std::byte, 2048> received{};
  for (int attempt = 0; attempt < 20; ++attempt) {
    auto result = receiver.receive_from(received, error);
    assert(!error);
    if (result.has_value()) {
      assert(result->bytes_received == packet.size());
      const auto parsed = udp_audio::protocol::parse_header(
        std::span<const std::byte, udp_audio::protocol::kHeaderSizeBytes>(received.data(),
                                                                         16));
      assert(parsed.sequence == 9);
      assert(parsed.timestamp_samples == frame.timestamp_samples);
      assert(parsed.payload_size == udp_audio::audio::kFramePayloadBytes);
      return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  assert(false && "loopback UDP packet was not received");
}

}  // namespace

int test_udp_socket_main() {
  udp_socket_sends_audio_packet_on_loopback();
  return 0;
}
