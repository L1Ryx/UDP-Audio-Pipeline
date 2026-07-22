#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace udp_audio::protocol {

inline constexpr std::uint16_t kMagic = 0x4155;  // "AU"
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSizeBytes = 16;
inline constexpr std::size_t kMaxPayloadBytes = 1200;

enum class PacketFlags : std::uint8_t {
  none = 0,
  fec_parity = 1 << 0,
  discontinuity = 1 << 1,
};

struct PacketHeader {
  std::uint16_t magic = kMagic;
  std::uint8_t version = kVersion;
  std::uint8_t flags = 0;
  std::uint32_t sequence = 0;
  std::uint32_t timestamp_samples = 0;
  std::uint16_t payload_size = 0;
  std::uint16_t header_crc = 0;
};

static_assert(sizeof(PacketHeader) == kHeaderSizeBytes);

using SerializedHeader = std::array<std::byte, kHeaderSizeBytes>;

SerializedHeader serialize_header(const PacketHeader& header) noexcept;
PacketHeader parse_header(std::span<const std::byte, kHeaderSizeBytes> bytes) noexcept;
bool is_valid_header(const PacketHeader& header) noexcept;

}  // namespace udp_audio::protocol

