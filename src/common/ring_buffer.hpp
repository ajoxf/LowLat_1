// Lock-free single-producer / single-consumer ring buffer.
//
// This is the single most important data structure in the system: one thread
// writes, one thread reads, and no locks are ever taken. Head and tail live on
// separate cache lines to avoid false sharing between producer and consumer.
#ifndef HFT_COMMON_RING_BUFFER_HPP_
#define HFT_COMMON_RING_BUFFER_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "common/compiler.hpp"

namespace hft {

// SPSC ring buffer templated on element type T and capacity Capacity.
// Capacity must be a power of two so that index wrapping is a cheap mask.
template <typename T, size_t Capacity>
class RingBuffer {
 public:
  static_assert(Capacity >= 2, "Capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T> || std::is_move_constructible_v<T>,
                "T must be copyable or movable");

  RingBuffer() : head_(0), tail_(0) {}

  RingBuffer(const RingBuffer&) = delete;
  RingBuffer& operator=(const RingBuffer&) = delete;

  // Producer side. Returns false (never blocks) if the buffer is full.
  HFT_ALWAYS_INLINE bool push(const T& value) {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    const uint64_t next = tail + 1;
    // Consumer publishes head with release; acquire here to observe space.
    if (HFT_UNLIKELY(next - head_.load(std::memory_order_acquire) > Capacity)) {
      return false;  // Full.
    }
    buffer_[tail & kMask] = value;
    tail_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns false (never blocks) if the buffer is empty.
  HFT_ALWAYS_INLINE bool pop(T& out) {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    if (HFT_UNLIKELY(head == tail_.load(std::memory_order_acquire))) {
      return false;  // Empty.
    }
    out = buffer_[head & kMask];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Approximate count of queued elements (safe to call from either side).
  HFT_ALWAYS_INLINE size_t size() const {
    return static_cast<size_t>(tail_.load(std::memory_order_acquire) -
                               head_.load(std::memory_order_acquire));
  }

  HFT_ALWAYS_INLINE bool empty() const { return size() == 0; }

  static constexpr size_t capacity() { return Capacity; }

 private:
  static constexpr uint64_t kMask = Capacity - 1;

  // Head and tail are padded to separate cache lines: the producer only writes
  // tail_ and the consumer only writes head_, so keeping them apart eliminates
  // false sharing.
  alignas(kCacheLineSize) std::atomic<uint64_t> head_;
  alignas(kCacheLineSize) std::atomic<uint64_t> tail_;
  alignas(kCacheLineSize) T buffer_[Capacity];
};

}  // namespace hft

#endif  // HFT_COMMON_RING_BUFFER_HPP_
