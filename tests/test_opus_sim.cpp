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

}  // namespace

int main() {
  robust_chirp_preset_recovers_losses_with_redundancy();
}
