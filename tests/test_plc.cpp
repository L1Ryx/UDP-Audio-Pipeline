#include "udp_audio/dsp/plc.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <span>

namespace {

bool nearly_equal(float lhs, float rhs) {
  return std::fabs(lhs - rhs) < 0.0001F;
}

void plc_repeats_and_decays() {
  udp_audio::dsp::HoldAndDecayPlc<4> plc;
  std::array<float, 4> good{1.0F, -1.0F, 0.5F, -0.5F};
  std::array<float, 4> concealed{};

  plc.accept_good_frame(std::span<const float, 4>(good));
  plc.synthesize_missing_frame(std::span<float, 4>(concealed));

  assert(nearly_equal(concealed[0], 1.0F));
  assert(nearly_equal(concealed[1], -1.0F));

  plc.synthesize_missing_frame(std::span<float, 4>(concealed));
  assert(nearly_equal(concealed[0], udp_audio::dsp::HoldAndDecayPlc<4>::kGainDecayPerLostFrame));
}

}  // namespace

int test_plc_main() {
  plc_repeats_and_decays();
  return 0;
}

