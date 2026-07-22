#include "udp_audio/protocol/packet.hpp"

#include <algorithm>

namespace udp_audio::protocol {
namespace {

void write_u16(SerializedHeader& out, std::size_t offset, std::uint16_t value) noexcept {
  out[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
  out[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void write_u32(SerializedHeader& out, std::size_t offset, std::uint32_t value) noexcept {
  out[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
  out[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
  out[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
  out[offset + 3U] = static_cast<std::byte>(value & 0xffU);
}

std::uint16_t read_u16(std::span<const std::byte, kHeaderSizeBytes> in,
                       std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
    (static_cast<std::uint16_t>(in[offset]) << 8U) | static_cast<std::uint16_t>(in[offset + 1U]));
}

std::uint32_t read_u32(std::span<const std::byte, kHeaderSizeBytes> in,
                       std::size_t offset) noexcept {
  return (static_cast<std::uint32_t>(in[offset]) << 24U) |
         (static_cast<std::uint32_t>(in[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(in[offset + 2U]) << 8U) |
         static_cast<std::uint32_t>(in[offset + 3U]);
}

}  // namespace

SerializedHeader serialize_header(const PacketHeader& header) noexcept {
  SerializedHeader out{};
  write_u16(out, 0, header.magic);
  out[2] = static_cast<std::byte>(header.version);
  out[3] = static_cast<std::byte>(header.flags);
  write_u32(out, 4, header.sequence);
  write_u32(out, 8, header.timestamp_samples);
  write_u16(out, 12, header.payload_size);
  write_u16(out, 14, header.header_crc);
  return out;
}

PacketHeader parse_header(std::span<const std::byte, kHeaderSizeBytes> bytes) noexcept {
  PacketHeader header{};
  header.magic = read_u16(bytes, 0);
  header.version = static_cast<std::uint8_t>(bytes[2]);
  header.flags = static_cast<std::uint8_t>(bytes[3]);
  header.sequence = read_u32(bytes, 4);
  header.timestamp_samples = read_u32(bytes, 8);
  header.payload_size = read_u16(bytes, 12);
  header.header_crc = read_u16(bytes, 14);
  return header;
}

bool is_valid_header(const PacketHeader& header) noexcept {
  return header.magic == kMagic && header.version == kVersion &&
         header.payload_size <= kMaxPayloadBytes;
}

}  // namespace udp_audio::protocol

