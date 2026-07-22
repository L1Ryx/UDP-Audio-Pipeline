#include "udp_audio/concurrency/spsc_ring_buffer.hpp"

#include <cassert>

namespace {

void spsc_push_pop_order() {
  udp_audio::concurrency::SpscRingBuffer<int, 4> queue;

  assert(queue.empty());
  assert(queue.capacity() == 3);
  assert(queue.push(1));
  assert(queue.push(2));
  assert(queue.push(3));
  assert(queue.full());
  assert(!queue.push(4));

  assert(queue.pop().value() == 1);
  assert(queue.pop().value() == 2);
  assert(queue.push(4));
  assert(queue.pop().value() == 3);
  assert(queue.pop().value() == 4);
  assert(!queue.pop().has_value());
}

}  // namespace

int test_spsc_main() {
  spsc_push_pop_order();
  return 0;
}

