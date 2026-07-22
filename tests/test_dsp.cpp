#include "udp_audio/dsp/gain.hpp"

#include <array>
#include <cassert>
#include <cmath>

namespace {

bool nearly_equal(float lhs, float rhs) {
  return std::fabs(lhs - rhs) < 0.0001F;
}

void gain_and_peak_work() {
  std::array<float, 5> samples{1.0F, -2.0F, 3.0F, -4.0F, 0.5F};
  udp_audio::dsp::apply_gain(samples, 0.25F);

  assert(nearly_equal(samples[0], 0.25F));
  assert(nearly_equal(samples[1], -0.5F));
  assert(nearly_equal(samples[2], 0.75F));
  assert(nearly_equal(samples[3], -1.0F));
  assert(nearly_equal(udp_audio::dsp::peak_abs(samples), 1.0F));
}

}  // namespace

int test_dsp_main() {
  gain_and_peak_work();
  return 0;
}

