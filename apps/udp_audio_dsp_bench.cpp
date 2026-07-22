#include "udp_audio/audio/frame.hpp"
#include "udp_audio/dsp/gain.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::size_t sample_count = udp_audio::audio::kFrameSamples * 1000U;
  std::size_t iterations = 200;
};

template <typename T>
bool parse_arg(std::string_view input, T& value) {
  const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
  return result.ec == std::errc{} && result.ptr == input.data() + input.size();
}

Options parse_options(int argc, char** argv) {
  Options options{};
  constexpr std::string_view kUsage =
    "Usage: udp_audio_dsp_bench [sample_count] [iterations]\n";

  if (argc > 1 && !parse_arg(std::string_view(argv[1]), options.sample_count)) {
    std::cerr << kUsage;
  }
  if (argc > 2 && !parse_arg(std::string_view(argv[2]), options.iterations)) {
    std::cerr << kUsage;
  }

  options.sample_count = std::max<std::size_t>(options.sample_count, 8U);
  options.iterations = std::max<std::size_t>(options.iterations, 1U);
  return options;
}

std::vector<float> make_signal(std::size_t sample_count) {
  std::vector<float> samples(sample_count);
  constexpr double kPi = 3.14159265358979323846;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double phase = (2.0 * kPi * static_cast<double>(i % 997U)) / 997.0;
    samples[i] = static_cast<float>(std::sin(phase) * 0.8);
  }
  return samples;
}

template <typename Func>
double measure_ms(std::size_t iterations, Func&& func) {
  const auto started = Clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    func(i);
  }
  return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

double throughput_msamples_per_second(std::size_t sample_count,
                                      std::size_t iterations,
                                      double elapsed_ms) {
  if (elapsed_ms <= 0.0) {
    return 0.0;
  }
  const double total_samples =
    static_cast<double>(sample_count) * static_cast<double>(iterations);
  return (total_samples / 1'000'000.0) / (elapsed_ms / 1000.0);
}

void print_metric(std::string_view key, double value) {
  std::cout << key << '=' << value << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  const auto source = make_signal(options.sample_count);
  auto scalar_samples = source;
  auto dispatch_samples = source;
  float scalar_peak_sink = 0.0F;
  float dispatch_peak_sink = 0.0F;

  const auto scalar_gain_ms = measure_ms(options.iterations, [&](std::size_t i) {
    const float gain = (i % 2U == 0U) ? 0.9999F : 1.0001F;
    udp_audio::dsp::apply_gain_scalar(std::span<float>(scalar_samples), gain);
  });

  const auto dispatch_gain_ms = measure_ms(options.iterations, [&](std::size_t i) {
    const float gain = (i % 2U == 0U) ? 0.9999F : 1.0001F;
    udp_audio::dsp::apply_gain(std::span<float>(dispatch_samples), gain);
  });

  const auto scalar_peak_ms = measure_ms(options.iterations, [&](std::size_t) {
    scalar_peak_sink += udp_audio::dsp::peak_abs_scalar(std::span<const float>(source));
  });

  const auto dispatch_peak_ms = measure_ms(options.iterations, [&](std::size_t) {
    dispatch_peak_sink += udp_audio::dsp::peak_abs(std::span<const float>(source));
  });

  const auto gain_speedup =
    dispatch_gain_ms > 0.0 ? scalar_gain_ms / dispatch_gain_ms : 0.0;
  const auto peak_speedup =
    dispatch_peak_ms > 0.0 ? scalar_peak_ms / dispatch_peak_ms : 0.0;

  std::cout << "summary\n";
  std::cout << "dispatch_backend=" << udp_audio::dsp::gain_dispatch_backend() << '\n';
  std::cout << "avx2_available=" << (udp_audio::dsp::gain_dispatch_uses_avx2() ? 1 : 0)
            << '\n';
  std::cout << "neon_available=" << (udp_audio::dsp::gain_dispatch_uses_neon() ? 1 : 0)
            << '\n';
  std::cout << "sample_count=" << options.sample_count << '\n';
  std::cout << "iterations=" << options.iterations << '\n';
  print_metric("scalar_apply_gain_ms", scalar_gain_ms);
  print_metric("dispatch_apply_gain_ms", dispatch_gain_ms);
  print_metric("apply_gain_speedup_vs_scalar", gain_speedup);
  print_metric("scalar_apply_gain_msamples_per_sec",
               throughput_msamples_per_second(options.sample_count, options.iterations,
                                               scalar_gain_ms));
  print_metric("dispatch_apply_gain_msamples_per_sec",
               throughput_msamples_per_second(options.sample_count, options.iterations,
                                               dispatch_gain_ms));
  print_metric("scalar_peak_abs_ms", scalar_peak_ms);
  print_metric("dispatch_peak_abs_ms", dispatch_peak_ms);
  print_metric("peak_abs_speedup_vs_scalar", peak_speedup);
  print_metric("scalar_peak_abs_msamples_per_sec",
               throughput_msamples_per_second(options.sample_count, options.iterations,
                                               scalar_peak_ms));
  print_metric("dispatch_peak_abs_msamples_per_sec",
               throughput_msamples_per_second(options.sample_count, options.iterations,
                                               dispatch_peak_ms));
  print_metric("sink", static_cast<double>(scalar_peak_sink + dispatch_peak_sink));
  return 0;
}
