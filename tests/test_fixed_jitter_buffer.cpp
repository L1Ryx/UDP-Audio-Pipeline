#include "udp_audio/jitter/fixed_jitter_buffer.hpp"
#include "udp_audio/jitter/adaptive_jitter_buffer.hpp"

#include <cassert>

namespace {

void fixed_jitter_buffer_reorders_by_expected_sequence() {
  udp_audio::jitter::FixedJitterBuffer<int, 8> buffer(3);

  assert(buffer.push(2, 20));
  assert(buffer.push(0, 0));
  assert(buffer.push(1, 10));

  assert(buffer.pop_expected(0).value() == 0);
  assert(buffer.pop_expected(1).value() == 10);
  assert(buffer.pop_expected(2).value() == 20);

  const auto& stats = buffer.stats();
  assert(stats.target_depth_frames == 3);
  assert(stats.inserted == 3);
  assert(stats.popped == 3);
  assert(stats.underruns == 0);
}

void fixed_jitter_buffer_counts_underruns() {
  udp_audio::jitter::FixedJitterBuffer<int, 8> buffer(2);

  assert(!buffer.pop_expected(7).has_value());
  assert(buffer.stats().underruns == 1);
}

void fixed_jitter_buffer_peeks_without_popping() {
  udp_audio::jitter::FixedJitterBuffer<int, 8> buffer(3);

  assert(buffer.push(4, 40));
  assert(buffer.peek_expected(4).value() == 40);
  assert(buffer.peek_expected(4).value() == 40);
  assert(buffer.pop_expected(4).value() == 40);
  assert(!buffer.peek_expected(4).has_value());

  const auto& stats = buffer.stats();
  assert(stats.inserted == 1);
  assert(stats.popped == 1);
  assert(stats.underruns == 0);
}

void adaptive_jitter_controller_increases_depth_on_late_gap() {
  udp_audio::jitter::AdaptiveJitterController controller(3, 8, 10.0);

  controller.observe_inter_arrival(10.0);
  assert(controller.stats().target_depth_frames == 3);

  controller.observe_inter_arrival(42.0);
  assert(controller.stats().target_depth_frames >= 6);
  assert(controller.stats().max_target_depth_frames >= 6);
  assert(controller.stats().observations == 2);
}

}  // namespace

int test_fixed_jitter_buffer_main() {
  fixed_jitter_buffer_reorders_by_expected_sequence();
  fixed_jitter_buffer_counts_underruns();
  fixed_jitter_buffer_peeks_without_popping();
  adaptive_jitter_controller_increases_depth_on_late_gap();
  return 0;
}
