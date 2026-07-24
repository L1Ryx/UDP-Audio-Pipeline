#include "udp_audio/sim/opus_playout_sim.hpp"

#include <cassert>

namespace {

void robust_chirp_preset_recovers_losses_with_redundancy() {
  udp_audio::sim::OpusSimulationSettings settings{};
  settings.frame_count = 100;
  settings.loss_percent = 20;
  settings.jitter_ms = 25;
  settings.seed = 1337;
  settings.bitrate_bps = 64'000;
  settings.redundancy_frames = 3;
  settings.jitter_depth_frames = 6;
  settings.source_mode = udp_audio::audio::SourceMode::chirp;
  settings.recovery_mode = udp_audio::sim::OpusRecoveryMode::fec;

  const auto result = udp_audio::sim::run_opus_playout_simulation(settings);

  assert(result.error.empty());
  assert(result.generated == settings.frame_count);
  assert(result.dropped == 23);
  assert(result.decoded == 99);
  assert(result.redundant == 22);
  assert(result.fec_attempts == 0);
  assert(result.plc == 1);
  assert(result.frames.back().status == udp_audio::sim::OpusFrameStatus::plc);
  assert(result.decode_errors == 0);
  assert(result.samples.size() ==
         static_cast<std::size_t>(settings.frame_count) * udp_audio::audio::kFrameSamples);
  assert(result.frames.size() == static_cast<std::size_t>(settings.frame_count));
}

void burst_loss_groups_drops_into_runs() {
  udp_audio::sim::OpusSimulationSettings settings{};
  settings.frame_count = 100;
  settings.loss_percent = 35;
  settings.jitter_ms = 20;
  settings.seed = 2026;
  settings.redundancy_frames = 3;
  settings.jitter_depth_frames = 8;
  settings.loss_model = udp_audio::sim::OpusLossModel::burst;
  settings.burst_min_frames = 3;
  settings.burst_max_frames = 6;
  settings.source_mode = udp_audio::audio::SourceMode::chirp;
  settings.recovery_mode = udp_audio::sim::OpusRecoveryMode::fec;

  const auto result = udp_audio::sim::run_opus_playout_simulation(settings);

  assert(result.error.empty());
  assert(result.generated == settings.frame_count);
  assert(result.dropped > 0);
  assert(result.loss_bursts > 0);
  assert(result.max_loss_burst_frames >= settings.burst_min_frames);
  assert(result.max_loss_burst_frames <= settings.burst_max_frames);
  assert(result.frames.size() == static_cast<std::size_t>(settings.frame_count));
  assert(result.samples.size() ==
         static_cast<std::size_t>(settings.frame_count) * udp_audio::audio::kFrameSamples);
}

void file_input_is_framed_before_opus_playout() {
  udp_audio::sim::OpusSimulationSettings settings{};
  settings.frame_count = 100;
  settings.loss_percent = 0;
  settings.jitter_ms = 0;
  settings.redundancy_frames = 0;
  settings.jitter_depth_frames = 3;
  settings.input_mode = udp_audio::sim::OpusInputMode::file;
  settings.input_label = "unit-test-input";
  settings.input_samples.resize((udp_audio::audio::kFrameSamples * 2U) + 1U);
  for (std::size_t i = 0; i < settings.input_samples.size(); ++i) {
    settings.input_samples[i] = static_cast<float>(static_cast<int>(i % 97U) - 48) / 200.0F;
  }

  const auto result = udp_audio::sim::run_opus_playout_simulation(settings);

  assert(result.error.empty());
  assert(result.generated == 3);
  assert(result.input_frames == 3);
  assert(result.padded_input_samples ==
         static_cast<int>(udp_audio::audio::kFrameSamples - 1U));
  assert(result.input_label == "unit-test-input");
  assert(result.dropped == 0);
  assert(result.plc == 0);
  assert(result.frames.size() == 3U);
  assert(result.samples.size() == 3U * udp_audio::audio::kFrameSamples);
}

}  // namespace

int main() {
  robust_chirp_preset_recovers_losses_with_redundancy();
  burst_loss_groups_drops_into_runs();
  file_input_is_framed_before_opus_playout();
}
