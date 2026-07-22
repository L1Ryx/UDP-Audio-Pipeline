#include "udp_audio/protocol/packet.hpp"

#include <array>
#include <cassert>
#include <cstddef>

namespace {

void packet_round_trips_header() {
  const udp_audio::protocol::PacketHeader header{
    .flags = 3,
    .sequence = 0x10203040,
    .timestamp_samples = 0x50607080,
    .payload_size = 960,
    .header_crc = 0x1111,
  };

  const auto bytes = udp_audio::protocol::serialize_header(header);
  const auto parsed = udp_audio::protocol::parse_header(std::span<const std::byte, 16>(bytes));

  assert(parsed.magic == udp_audio::protocol::kMagic);
  assert(parsed.version == udp_audio::protocol::kVersion);
  assert(parsed.flags == header.flags);
  assert(parsed.sequence == header.sequence);
  assert(parsed.timestamp_samples == header.timestamp_samples);
  assert(parsed.payload_size == header.payload_size);
  assert(parsed.header_crc == header.header_crc);
  assert(udp_audio::protocol::is_valid_header(parsed));
}

}  // namespace

int main();

int test_packet_main() {
  packet_round_trips_header();
  return 0;
}

