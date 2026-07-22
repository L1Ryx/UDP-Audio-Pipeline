#pragma once

#include "udp_audio/audio/frame.hpp"

#include <cstdint>
#include <fstream>
#include <string>

namespace udp_audio::audio {

class WavWriter {
 public:
  bool open(const std::string& path);
  void write_frame(const MonoAudioFrame& frame);
  void close();

  [[nodiscard]] std::uint64_t recorded_frames() const noexcept;
  [[nodiscard]] std::uint64_t data_bytes() const noexcept;
  [[nodiscard]] const std::string& path() const noexcept;

 private:
  void write_u16(std::uint16_t value);
  void write_u32(std::uint32_t value);
  void write_header(std::uint64_t data_bytes);

  std::ofstream output_{};
  std::string path_{};
  std::uint64_t data_bytes_ = 0;
  std::uint64_t recorded_frames_ = 0;
};

}  // namespace udp_audio::audio
