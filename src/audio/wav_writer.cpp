#include "udp_audio/audio/wav_writer.hpp"

#include <algorithm>
#include <ios>

namespace udp_audio::audio {

bool WavWriter::open(const std::string& path) {
  output_.open(path, std::ios::binary);
  if (!output_) {
    return false;
  }

  data_bytes_ = 0;
  recorded_frames_ = 0;
  write_header(0);
  path_ = path;
  return output_.good();
}

void WavWriter::write_frame(const MonoAudioFrame& frame) {
  if (!output_) {
    return;
  }

  const auto bytes = static_cast<std::streamsize>(kFramePayloadBytes);
  output_.write(reinterpret_cast<const char*>(frame.samples.data()), bytes);
  data_bytes_ += kFramePayloadBytes;
  ++recorded_frames_;
}

void WavWriter::close() {
  if (!output_) {
    return;
  }

  output_.seekp(0, std::ios::beg);
  write_header(data_bytes_);
  output_.close();
}

std::uint64_t WavWriter::recorded_frames() const noexcept {
  return recorded_frames_;
}

std::uint64_t WavWriter::data_bytes() const noexcept {
  return data_bytes_;
}

const std::string& WavWriter::path() const noexcept {
  return path_;
}

void WavWriter::write_u16(std::uint16_t value) {
  const char bytes[] = {
    static_cast<char>(value & 0xffU),
    static_cast<char>((value >> 8U) & 0xffU),
  };
  output_.write(bytes, sizeof(bytes));
}

void WavWriter::write_u32(std::uint32_t value) {
  const char bytes[] = {
    static_cast<char>(value & 0xffU),
    static_cast<char>((value >> 8U) & 0xffU),
    static_cast<char>((value >> 16U) & 0xffU),
    static_cast<char>((value >> 24U) & 0xffU),
  };
  output_.write(bytes, sizeof(bytes));
}

void WavWriter::write_header(std::uint64_t data_bytes) {
  constexpr std::uint16_t kAudioFormatIeeeFloat = 3;
  constexpr std::uint16_t kBitsPerSample = 32;
  constexpr std::uint16_t kBlockAlign = static_cast<std::uint16_t>(kChannels * sizeof(float));
  constexpr std::uint32_t kByteRate = kSampleRateHz * kBlockAlign;

  const auto clamped_data_bytes =
    static_cast<std::uint32_t>(std::min<std::uint64_t>(data_bytes, 0xfffffff0ULL));

  output_.write("RIFF", 4);
  write_u32(36U + clamped_data_bytes);
  output_.write("WAVE", 4);

  output_.write("fmt ", 4);
  write_u32(16);
  write_u16(kAudioFormatIeeeFloat);
  write_u16(static_cast<std::uint16_t>(kChannels));
  write_u32(kSampleRateHz);
  write_u32(kByteRate);
  write_u16(kBlockAlign);
  write_u16(kBitsPerSample);

  output_.write("data", 4);
  write_u32(clamped_data_bytes);
}

}  // namespace udp_audio::audio
