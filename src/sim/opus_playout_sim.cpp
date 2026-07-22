#include "udp_audio/sim/opus_playout_sim.hpp"

#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <random>

namespace udp_audio::sim {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxOpusPacketBytes = 1500;
constexpr std::size_t kMaxPlotSamples = 2400;
constexpr std::size_t kFrameSamples = audio::kFrameSamples;
constexpr std::size_t kSampleRateHz = audio::kSampleRateHz;
constexpr std::size_t kFrameDurationMs = audio::kFrameDurationMs;

struct EncodedFrame {
  std::array<unsigned char, kMaxOpusPacketBytes> payload{};
  std::size_t payload_size = 0;
};

bool encode_frames(const OpusSimulationSettings& settings,
                   std::vector<EncodedFrame>& encoded_frames,
                   int encode_frame_count,
                   OpusSimulationResult& result) {
  int opus_error = OPUS_OK;
  OpusEncoder* encoder =
    opus_encoder_create(static_cast<opus_int32>(kSampleRateHz), 1, OPUS_APPLICATION_AUDIO,
                        &opus_error);
  if (opus_error != OPUS_OK || encoder == nullptr) {
    result.error = std::string("Opus encoder: ") + opus_strerror(opus_error);
    return false;
  }

  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(settings.bitrate_bps));
  opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
  opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(settings.loss_percent));
  if (settings.recovery_mode == OpusRecoveryMode::fec) {
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
  }

  audio::SourceState source_state{};
  encoded_frames.resize(static_cast<std::size_t>(encode_frame_count));
  for (int i = 0; i < encode_frame_count; ++i) {
    const auto frame =
      audio::make_source_frame(settings.source_mode, static_cast<std::uint32_t>(i),
                               static_cast<std::size_t>(encode_frame_count), source_state);
    auto& encoded = encoded_frames[static_cast<std::size_t>(i)];
    const auto byte_count = opus_encode_float(
      encoder, frame.samples.data(), static_cast<int>(kFrameSamples), encoded.payload.data(),
      static_cast<opus_int32>(encoded.payload.size()));
    if (byte_count < 0) {
      result.error = std::string("Opus encode: ") + opus_strerror(byte_count);
      opus_encoder_destroy(encoder);
      return false;
    }
    encoded.payload_size = static_cast<std::size_t>(byte_count);
    result.avg_packet_bytes += static_cast<double>(encoded.payload_size);
  }

  opus_encoder_destroy(encoder);
  result.avg_packet_bytes /= std::max(1, encode_frame_count);
  return true;
}

void append_frame_metrics(const audio::MonoAudioFrame& frame, OpusFrameReport& report) {
  float peak = 0.0F;
  double energy = 0.0;
  for (const float sample : frame.samples) {
    peak = std::max(peak, std::abs(sample));
    energy += static_cast<double>(sample) * static_cast<double>(sample);
  }
  report.peak = peak;
  report.rms = static_cast<float>(std::sqrt(energy / static_cast<double>(frame.samples.size())));
}

bool decode_packet(OpusDecoder* decoder,
                   const EncodedFrame& encoded,
                   audio::MonoAudioFrame& frame,
                   OpusSimulationResult& result) {
  const auto decoded = opus_decode_float(
    decoder, encoded.payload.data(), static_cast<opus_int32>(encoded.payload_size),
    frame.samples.data(), static_cast<int>(kFrameSamples), 0);
  if (decoded != static_cast<int>(kFrameSamples)) {
    ++result.decode_errors;
    std::fill(frame.samples.begin(), frame.samples.end(), 0.0F);
    return false;
  }
  return true;
}

bool decode_fec_attempt(OpusDecoder* decoder,
                        const EncodedFrame& encoded,
                        audio::MonoAudioFrame& frame,
                        OpusSimulationResult& result) {
  const auto decoded = opus_decode_float(
    decoder, encoded.payload.data(), static_cast<opus_int32>(encoded.payload_size),
    frame.samples.data(), static_cast<int>(kFrameSamples), 1);
  if (decoded != static_cast<int>(kFrameSamples)) {
    ++result.decode_errors;
    std::fill(frame.samples.begin(), frame.samples.end(), 0.0F);
    return false;
  }
  return true;
}

void decode_plc(OpusDecoder* decoder,
                audio::MonoAudioFrame& frame,
                OpusSimulationResult& result) {
  const auto decoded = opus_decode_float(decoder, nullptr, 0, frame.samples.data(),
                                         static_cast<int>(kFrameSamples), 0);
  if (decoded != static_cast<int>(kFrameSamples)) {
    ++result.decode_errors;
    std::fill(frame.samples.begin(), frame.samples.end(), 0.0F);
  }
}

}  // namespace

OpusSimulationResult run_opus_playout_simulation(const OpusSimulationSettings& settings) {
  const auto started = Clock::now();
  OpusSimulationResult result{};
  result.generated = settings.frame_count;

  const int repair_tail_frames =
    std::clamp(settings.redundancy_frames, 0, kMaxOpusRedundancyFrames);
  const int encoded_frame_count = settings.frame_count + repair_tail_frames;

  std::vector<EncodedFrame> encoded_frames;
  if (!encode_frames(settings, encoded_frames, encoded_frame_count, result)) {
    return result;
  }

  std::mt19937 rng(static_cast<std::uint32_t>(settings.seed));
  std::uniform_int_distribution<int> loss_distribution(1, 100);
  std::uniform_int_distribution<int> jitter_distribution(0, std::max(0, settings.jitter_ms));
  std::vector<bool> dropped(static_cast<std::size_t>(encoded_frame_count), false);
  std::vector<double> arrival_ms(static_cast<std::size_t>(encoded_frame_count), 0.0);

  for (int i = 0; i < encoded_frame_count; ++i) {
    dropped[static_cast<std::size_t>(i)] =
      i < settings.frame_count && settings.loss_percent > 0 &&
      loss_distribution(rng) <= settings.loss_percent;
    const double jitter_ms =
      !dropped[static_cast<std::size_t>(i)] && settings.jitter_ms > 0
        ? jitter_distribution(rng)
        : 0.0;
    arrival_ms[static_cast<std::size_t>(i)] =
      static_cast<double>(i * static_cast<int>(kFrameDurationMs)) + jitter_ms;
    if (i < settings.frame_count && dropped[static_cast<std::size_t>(i)]) {
      ++result.dropped;
    }
  }

  int opus_error = OPUS_OK;
  OpusDecoder* decoder =
    opus_decoder_create(static_cast<opus_int32>(kSampleRateHz), 1, &opus_error);
  if (opus_error != OPUS_OK || decoder == nullptr) {
    result.error = std::string("Opus decoder: ") + opus_strerror(opus_error);
    return result;
  }

  result.frames.resize(static_cast<std::size_t>(settings.frame_count));
  result.samples.reserve(static_cast<std::size_t>(settings.frame_count) * kFrameSamples);

  for (int sequence = 0; sequence < settings.frame_count; ++sequence) {
    const double playout_ms =
      static_cast<double>((sequence + settings.jitter_depth_frames) *
                          static_cast<int>(kFrameDurationMs));
    OpusFrameReport report{};
    report.playout_latency_ms =
      static_cast<double>(settings.jitter_depth_frames * static_cast<int>(kFrameDurationMs));
    audio::MonoAudioFrame output{};
    output.sequence = static_cast<std::uint32_t>(sequence);
    output.timestamp_samples = output.sequence * static_cast<std::uint32_t>(kFrameSamples);

    const auto seq_index = static_cast<std::size_t>(sequence);
    const bool primary_available =
      !dropped[seq_index] && arrival_ms[seq_index] <= playout_ms;
    if (primary_available) {
      decode_packet(decoder, encoded_frames[seq_index], output, result);
      report.status = OpusFrameStatus::decoded;
      report.network_latency_ms =
        arrival_ms[seq_index] - static_cast<double>(sequence * static_cast<int>(kFrameDurationMs));
      ++result.decoded;
    } else {
      bool recovered = false;
      for (int lookahead = 1; lookahead <= settings.redundancy_frames; ++lookahead) {
        const int carrier = sequence + lookahead;
        if (carrier >= encoded_frame_count) {
          break;
        }
        const auto carrier_index = static_cast<std::size_t>(carrier);
        if (!dropped[carrier_index] && arrival_ms[carrier_index] <= playout_ms) {
          decode_packet(decoder, encoded_frames[seq_index], output, result);
          report.status = OpusFrameStatus::redundant;
          ++result.decoded;
          ++result.redundant;
          recovered = true;
          break;
        }
      }

      if (!recovered && settings.recovery_mode == OpusRecoveryMode::fec &&
          sequence + 1 < encoded_frame_count) {
        const auto carrier_index = static_cast<std::size_t>(sequence + 1);
        if (!dropped[carrier_index] && arrival_ms[carrier_index] <= playout_ms) {
          decode_fec_attempt(decoder, encoded_frames[carrier_index], output, result);
          report.status = OpusFrameStatus::fec_attempt;
          ++result.fec_attempts;
          recovered = true;
        }
      }

      if (!recovered) {
        decode_plc(decoder, output, result);
        report.status = OpusFrameStatus::plc;
        ++result.plc;
      }

      ++result.late_or_missing;
    }

    append_frame_metrics(output, report);
    result.frame_energy.push_back(report.rms);
    result.samples.insert(result.samples.end(), output.samples.begin(), output.samples.end());
    result.frames[seq_index] = report;

    result.avg_latency_ms += report.network_latency_ms;
    result.max_latency_ms = std::max(result.max_latency_ms, report.network_latency_ms);
    result.avg_playout_ms += report.playout_latency_ms;
    result.max_playout_ms = std::max(result.max_playout_ms, report.playout_latency_ms);
  }

  opus_decoder_destroy(decoder);

  result.avg_latency_ms /= std::max(1, result.decoded);
  result.avg_playout_ms /= std::max(1, settings.frame_count);
  result.avg_redundancy_bytes =
    result.avg_packet_bytes * static_cast<double>(settings.redundancy_frames);

  const auto stride =
    std::max<std::size_t>(1U, result.samples.size() / kMaxPlotSamples);
  for (std::size_t i = 0; i < result.samples.size(); i += stride) {
    result.waveform_plot.push_back(result.samples[i]);
  }

  result.elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  return result;
}

const char* status_name(OpusFrameStatus status) noexcept {
  switch (status) {
    case OpusFrameStatus::decoded:
      return "decoded";
    case OpusFrameStatus::redundant:
      return "redundant";
    case OpusFrameStatus::fec_attempt:
      return "fec";
    case OpusFrameStatus::plc:
      return "plc";
  }
  return "unknown";
}

}  // namespace udp_audio::sim
