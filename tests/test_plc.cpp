#include "udp_audio/dsp/plc.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <span>

namespace {

bool nearly_equal(float lhs, float rhs) {
  return std::fabs(lhs - rhs) < 0.0001F;
}

template <std::size_t Samples>
void fill_sine(std::array<float, Samples>& frame, double& phase) {
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kSampleRateHz = 48000.0;
  constexpr double kToneHz = 440.0;
  constexpr float kGain = 0.2F;
  const double phase_step = (2.0 * kPi * kToneHz) / kSampleRateHz;

  for (auto& sample : frame) {
    sample = static_cast<float>(std::sin(phase) * kGain);
    phase += phase_step;
    if (phase >= 2.0 * kPi) {
      phase -= 2.0 * kPi;
    }
  }
}

template <std::size_t Samples>
float rms_error(const std::array<float, Samples>& actual, const std::array<float, Samples>& expected) {
  float sum = 0.0F;
  for (std::size_t i = 0; i < Samples; ++i) {
    const float error = actual[i] - expected[i];
    sum += error * error;
  }
  return std::sqrt(sum / static_cast<float>(Samples));
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

void periodic_interpolation_tracks_fractional_periods() {
  constexpr std::size_t kSamples = 128;
  udp_audio::dsp::HoldAndDecayPlc<kSamples> integer_plc;
  udp_audio::dsp::HoldAndDecayPlc<kSamples> interpolated_plc;
  std::array<float, kSamples> good{};
  std::array<float, kSamples> clean_next{};
  std::array<float, kSamples> integer_concealed{};
  std::array<float, kSamples> interpolated_concealed{};
  double phase = 0.0;

  for (int i = 0; i < 4; ++i) {
    fill_sine(good, phase);
    integer_plc.accept_good_frame(std::span<const float, kSamples>(good));
    interpolated_plc.accept_good_frame(std::span<const float, kSamples>(good));
  }
  fill_sine(clean_next, phase);

  integer_plc.synthesize_missing_frame(std::span<float, kSamples>(integer_concealed),
                                       udp_audio::dsp::PlcSynthesisMode::periodic);
  interpolated_plc.synthesize_missing_frame(
    std::span<float, kSamples>(interpolated_concealed),
    udp_audio::dsp::PlcSynthesisMode::periodic_interpolated);

  assert(rms_error(interpolated_concealed, clean_next) <
         rms_error(integer_concealed, clean_next));
}

}  // namespace

int test_plc_main() {
  plc_repeats_and_decays();
  periodic_interpolation_tracks_fractional_periods();
  return 0;
}
