#pragma once

#include "udp_audio/audio/source.hpp"
#include "udp_audio/codec/opus_packet.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace udp_audio::sim {

enum class OpusRecoveryMode {
  plc,
  fec,
};

enum class OpusFrameStatus {
  decoded,
  redundant,
  fec_attempt,
  plc,
};

struct OpusSimulationSettings {
  int frame_count = 100;
  int loss_percent = 20;
  int jitter_ms = 25;
  int seed = 1337;
  int bitrate_bps = 64000;
  int redundancy_frames = 3;
  int jitter_depth_frames = 6;
  audio::SourceMode source_mode = audio::SourceMode::chirp;
  OpusRecoveryMode recovery_mode = OpusRecoveryMode::fec;
};

struct OpusFrameReport {
  OpusFrameStatus status = OpusFrameStatus::decoded;
  double network_latency_ms = 0.0;
  double playout_latency_ms = 0.0;
  float rms = 0.0F;
  float peak = 0.0F;
};

struct OpusSimulationResult {
  std::vector<float> samples;
  std::vector<float> waveform_plot;
  std::vector<float> frame_energy;
  std::vector<OpusFrameReport> frames;
  std::string error;
  int generated = 0;
  int dropped = 0;
  int decoded = 0;
  int redundant = 0;
  int fec_attempts = 0;
  int plc = 0;
  int late_or_missing = 0;
  int decode_errors = 0;
  double avg_latency_ms = 0.0;
  double max_latency_ms = 0.0;
  double avg_playout_ms = 0.0;
  double max_playout_ms = 0.0;
  double elapsed_ms = 0.0;
  double avg_packet_bytes = 0.0;
  double avg_redundancy_bytes = 0.0;
};

inline constexpr int kMaxOpusRedundancyFrames =
  static_cast<int>(codec::kMaxRedundantOpusFrames);

OpusSimulationResult run_opus_playout_simulation(const OpusSimulationSettings& settings);

const char* status_name(OpusFrameStatus status) noexcept;

}  // namespace udp_audio::sim
