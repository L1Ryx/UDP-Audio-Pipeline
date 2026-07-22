#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace udp_audio::codec {

inline constexpr std::size_t kMaxOpusPacketBytes = 1500;
inline constexpr std::size_t kMaxRedundantOpusFrames = 3;

struct EncodedOpusPacket {
  std::array<unsigned char, kMaxOpusPacketBytes> payload{};
  std::size_t payload_size = 0;
  std::uint32_t sequence = 0;
  std::uint32_t timestamp_samples = 0;
};

struct OpusPacketBundle {
  EncodedOpusPacket primary{};
  std::array<EncodedOpusPacket, kMaxRedundantOpusFrames> redundant_packets{};
  std::size_t redundant_packet_count = 0;
};

struct SerializedOpusBundle {
  std::vector<std::byte> payload;
  std::size_t redundancy_bytes = 0;
  std::size_t redundant_packet_count = 0;
};

SerializedOpusBundle serialize_opus_bundle(
  const EncodedOpusPacket& primary,
  std::span<const EncodedOpusPacket> redundancy_history_oldest_first,
  std::size_t requested_redundant_frames);

std::optional<OpusPacketBundle> parse_opus_bundle(std::span<const std::byte> payload,
                                                  std::uint32_t primary_sequence,
                                                  std::uint32_t primary_timestamp_samples);

}  // namespace udp_audio::codec
