#include "udp_audio/codec/opus_packet.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace {

udp_audio::codec::EncodedOpusPacket make_encoded(std::uint32_t sequence,
                                                 std::initializer_list<unsigned char> bytes) {
  udp_audio::codec::EncodedOpusPacket packet{};
  packet.sequence = sequence;
  packet.timestamp_samples = sequence * 480U;
  packet.payload_size = bytes.size();
  std::copy(bytes.begin(), bytes.end(), packet.payload.begin());
  return packet;
}

void opus_bundle_round_trips_primary_and_recent_repairs() {
  const auto primary = make_encoded(12, {0x12, 0x13, 0x14});
  const std::array history{
    make_encoded(9, {0x90}),
    make_encoded(10, {0xa0, 0xa1}),
    make_encoded(11, {0xb0, 0xb1, 0xb2}),
  };

  const auto serialized =
    udp_audio::codec::serialize_opus_bundle(primary, std::span<const udp_audio::codec::EncodedOpusPacket>(history), 2);
  assert(serialized.redundant_packet_count == 2);
  assert(serialized.redundancy_bytes == 5);

  const auto parsed =
    udp_audio::codec::parse_opus_bundle(serialized.payload, primary.sequence,
                                        primary.timestamp_samples);
  assert(parsed.has_value());
  assert(parsed->primary.sequence == 12);
  assert(parsed->primary.timestamp_samples == 12U * 480U);
  assert(parsed->primary.payload_size == 3);
  assert(parsed->primary.payload[0] == 0x12);
  assert(parsed->primary.payload[2] == 0x14);

  assert(parsed->redundant_packet_count == 2);
  assert(parsed->redundant_packets[0].sequence == 11);
  assert(parsed->redundant_packets[0].payload_size == 3);
  assert(parsed->redundant_packets[0].payload[2] == 0xb2);
  assert(parsed->redundant_packets[1].sequence == 10);
  assert(parsed->redundant_packets[1].payload_size == 2);
  assert(parsed->redundant_packets[1].payload[1] == 0xa1);
}

void opus_bundle_rejects_invalid_header() {
  std::vector<std::byte> payload{std::byte{0x00}, std::byte{0x01}, std::byte{0x00}};
  assert(!udp_audio::codec::parse_opus_bundle(payload, 0, 0).has_value());
}

}  // namespace

int test_opus_packet_main() {
  opus_bundle_round_trips_primary_and_recent_repairs();
  opus_bundle_rejects_invalid_header();
  return 0;
}
