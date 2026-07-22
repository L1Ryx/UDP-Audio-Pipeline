#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace udp_audio::concurrency {

template <typename T, std::size_t Capacity>
class SpscRingBuffer {
  static_assert(Capacity >= 2);
  static_assert(std::has_single_bit(Capacity), "Capacity must be a power of two");
  static_assert(std::is_default_constructible_v<T>);

 public:
  bool push(const T& value) noexcept(std::is_nothrow_copy_assignable_v<T>) {
    const auto write_index = write_index_.load(std::memory_order_relaxed);
    const auto next = increment(write_index);

    if (next == read_index_.load(std::memory_order_acquire)) {
      return false;
    }

    storage_[write_index] = value;
    write_index_.store(next, std::memory_order_release);
    return true;
  }

  std::optional<T> pop() noexcept(std::is_nothrow_copy_constructible_v<T>) {
    const auto read_index = read_index_.load(std::memory_order_relaxed);

    if (read_index == write_index_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }

    auto value = storage_[read_index];
    read_index_.store(increment(read_index), std::memory_order_release);
    return value;
  }

  [[nodiscard]] bool empty() const noexcept {
    return read_index_.load(std::memory_order_acquire) ==
           write_index_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool full() const noexcept {
    const auto next = increment(write_index_.load(std::memory_order_acquire));
    return next == read_index_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return Capacity - 1U;
  }

 private:
  static constexpr std::size_t increment(std::size_t index) noexcept {
    return (index + 1U) & (Capacity - 1U);
  }

  alignas(64) std::array<T, Capacity> storage_{};
  alignas(64) std::atomic<std::size_t> read_index_{0};
  alignas(64) std::atomic<std::size_t> write_index_{0};
};

}  // namespace udp_audio::concurrency

