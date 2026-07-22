#include "udp_audio/codec/opus_packet.hpp"

#include "udp_audio/protocol/packet.hpp"

#include <algorithm>
#include <cstring>

namespace udp_audio::codec {
namespace {

constexpr std::uint8_t kBundleMagic = 0x4f;  // "O"
constexpr std::uint8_t kBundleVersion = 1;
constexpr std::size_t kBundleHeaderBytes = 6;
constexpr std::size_t kRepairDescriptorBytes = 10;

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

void append_packet_payload(std::vector<std::byte>& payload, const EncodedOpusPacket& packet) {
  const auto* begin = reinterpret_cast<const std::byte*>(packet.payload.data());
  payload.insert(payload.end(), begin, begin + packet.payload_size);
}

}  // namespace

SerializedOpusBundle serialize_opus_bundle(
  const EncodedOpusPacket& primary,
  std::span<const EncodedOpusPacket> redundancy_history_oldest_first,
  std::size_t requested_redundant_frames) {
  SerializedOpusBundle serialized{};

  const auto requested_count =
    std::min({requested_redundant_frames, redundancy_history_oldest_first.size(),
              kMaxRedundantOpusFrames});
  std::size_t projected_payload_size = kBundleHeaderBytes + primary.payload_size;

  for (std::size_t i = 0; i < requested_count; ++i) {
    const auto& repair =
      redundancy_history_oldest_first[redundancy_history_oldest_first.size() - 1U - i];
    const auto next_size = projected_payload_size + kRepairDescriptorBytes + repair.payload_size;
    if (next_size > protocol::kMaxPayloadBytes) {
      break;
    }
    projected_payload_size = next_size;
    ++serialized.redundant_packet_count;
  }

  serialized.payload.reserve(projected_payload_size);
  append_u8(serialized.payload, kBundleMagic);
  append_u8(serialized.payload, kBundleVersion);
  append_u8(serialized.payload, static_cast<std::uint8_t>(serialized.redundant_packet_count));
  append_u8(serialized.payload, 0);
  append_u16(serialized.payload, static_cast<std::uint16_t>(primary.payload_size));

  for (std::size_t i = 0; i < serialized.redundant_packet_count; ++i) {
    const auto& repair =
      redundancy_history_oldest_first[redundancy_history_oldest_first.size() - 1U - i];
    append_u32(serialized.payload, repair.sequence);
    append_u32(serialized.payload, repair.timestamp_samples);
    append_u16(serialized.payload, static_cast<std::uint16_t>(repair.payload_size));
    serialized.redundancy_bytes += repair.payload_size;
  }

  append_packet_payload(serialized.payload, primary);
  for (std::size_t i = 0; i < serialized.redundant_packet_count; ++i) {
    append_packet_payload(
      serialized.payload,
      redundancy_history_oldest_first[redundancy_history_oldest_first.size() - 1U - i]);
  }

  return serialized;
}

std::optional<OpusPacketBundle> parse_opus_bundle(std::span<const std::byte> payload,
                                                  std::uint32_t primary_sequence,
                                                  std::uint32_t primary_timestamp_samples) {
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

  OpusPacketBundle bundle{};
  bundle.primary.sequence = primary_sequence;
  bundle.primary.timestamp_samples = primary_timestamp_samples;
  bundle.primary.payload_size = primary_size;
  std::memcpy(bundle.primary.payload.data(), payload.data() + payload_header_bytes, primary_size);

  auto blob_offset = payload_header_bytes + primary_size;
  for (std::size_t i = 0; i < repair_count; ++i) {
    const auto descriptor_offset = kBundleHeaderBytes + (i * kRepairDescriptorBytes);
    auto& repair = bundle.redundant_packets[i];
    repair.sequence = read_u32(payload, descriptor_offset);
    repair.timestamp_samples = read_u32(payload, descriptor_offset + 4U);
    repair.payload_size = read_u16(payload, descriptor_offset + 8U);
    if (repair.payload_size == 0 || repair.payload_size > kMaxOpusPacketBytes ||
        payload.size() < blob_offset + repair.payload_size) {
      return std::nullopt;
    }
    std::memcpy(repair.payload.data(), payload.data() + blob_offset, repair.payload_size);
    blob_offset += repair.payload_size;
    ++bundle.redundant_packet_count;
  }

  return bundle;
}

}  // namespace udp_audio::codec
