#include "udp_audio/concurrency/spsc_ring_buffer.hpp"
#include "udp_audio/dsp/gain.hpp"
#include "udp_audio/protocol/packet.hpp"
#include "udp_audio/version.hpp"

#include <array>
#include <cstddef>
#include <iostream>

int main() {
  using udp_audio::concurrency::SpscRingBuffer;
  using udp_audio::protocol::PacketHeader;

  SpscRingBuffer<int, 8> queue;
  queue.push(42);

  std::array<float, 4> samples{0.25F, -0.5F, 0.75F, -1.0F};
  udp_audio::dsp::apply_gain(samples, 0.5F);

  const PacketHeader header{.sequence = 7, .timestamp_samples = 480, .payload_size = 960};
  const auto bytes = udp_audio::protocol::serialize_header(header);
  const auto parsed = udp_audio::protocol::parse_header(std::span<const std::byte, 16>(bytes));

  std::cout << "udp_audio_pipeline " << udp_audio::version() << '\n';
  std::cout << "queue_pop=" << queue.pop().value_or(-1) << '\n';
  std::cout << "peak=" << udp_audio::dsp::peak_abs(samples) << '\n';
  std::cout << "packet_sequence=" << parsed.sequence << '\n';
}

