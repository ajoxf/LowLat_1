// Milestone 1 tests: ring buffer, RDTSC clock, latency tracker, memory pool,
// and IST time helpers.
#include "common/ring_buffer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "common/memory_pool.hpp"
#include "common/time_utils.hpp"

namespace hft {
namespace {

// 1. push() returns false when the buffer is full.
TEST(RingBuffer, PushFailsWhenFull) {
  RingBuffer<int, 4> rb;
  EXPECT_TRUE(rb.push(1));
  EXPECT_TRUE(rb.push(2));
  EXPECT_TRUE(rb.push(3));
  EXPECT_TRUE(rb.push(4));
  EXPECT_FALSE(rb.push(5));  // Full at capacity.
}

// 2. pop() returns false when the buffer is empty.
TEST(RingBuffer, PopFailsWhenEmpty) {
  RingBuffer<int, 4> rb;
  int out = 0;
  EXPECT_FALSE(rb.pop(out));
}

// 3. One million sequential push/pop ops preserve order.
TEST(RingBuffer, SequentialMillionInOrder) {
  RingBuffer<uint64_t, 1024> rb;
  uint64_t expected = 0;
  for (uint64_t i = 0; i < 1'000'000; ++i) {
    ASSERT_TRUE(rb.push(i));
    uint64_t out = 0;
    ASSERT_TRUE(rb.pop(out));
    EXPECT_EQ(out, expected);
    ++expected;
  }
  EXPECT_EQ(expected, 1'000'000u);
}

// 4. Two threads for several seconds produce zero lost messages.
TEST(RingBuffer, ConcurrentNoLoss) {
  RingBuffer<uint64_t, 4096> rb;
  constexpr uint64_t kCount = 5'000'000;
  std::atomic<bool> start{false};

  std::thread producer([&] {
    while (!start.load()) {
    }
    for (uint64_t i = 0; i < kCount; ++i) {
      while (!rb.push(i)) {
      }
    }
  });

  uint64_t received = 0;
  std::thread consumer([&] {
    while (!start.load()) {
    }
    uint64_t out = 0;
    uint64_t expected = 0;
    while (received < kCount) {
      if (rb.pop(out)) {
        ASSERT_EQ(out, expected);
        ++expected;
        ++received;
      }
    }
  });

  start.store(true);
  producer.join();
  consumer.join();
  EXPECT_EQ(received, kCount);
}

// 5 & 6. Head and tail are on separate cache lines; total size accounting.
TEST(RingBuffer, CacheLineLayout) {
  RingBuffer<uint64_t, 1024> rb;
  // Two atomics each padded to a 64-byte line + the buffer line-aligned.
  // The two index lines contribute exactly 128 bytes of overhead.
  EXPECT_EQ(sizeof(rb), sizeof(uint64_t) * 1024 + 128);
}

// 7. RDTSC is monotonically non-decreasing across 1000 calls.
TEST(Rdtsc, Monotonic) {
  uint64_t prev = rdtsc();
  for (int i = 0; i < 1000; ++i) {
    uint64_t now = rdtsc();
    EXPECT_GE(now, prev);
    prev = now;
  }
}

// 8. TSC calibration produces a sane positive frequency.
TEST(Rdtsc, CalibrationSane) {
  const uint64_t hz = TscClock::Instance().ticks_per_second();
  // Any modern CPU is between 0.5 GHz and 10 GHz; the fallback clock is 1 GHz.
  EXPECT_GE(hz, 500'000'000u);
  EXPECT_LE(hz, 10'000'000'000u);
}

// 9. LatencyTracker computes percentiles correctly over a known distribution.
TEST(LatencyTracker, Percentiles) {
  LatencyTracker lt(2'000'000);
  for (uint64_t i = 1; i <= 1'000'000; ++i) {
    lt.record(i);  // Uniform 1..1,000,000.
  }
  EXPECT_EQ(lt.count(), 1'000'000u);
  EXPECT_EQ(lt.min(), 1u);
  EXPECT_EQ(lt.max(), 1'000'000u);
  // p99 ~ 990,000 and p999 ~ 999,000 (allow small rounding slack).
  EXPECT_NEAR(static_cast<double>(lt.p99()), 990'000.0, 50.0);
  EXPECT_NEAR(static_cast<double>(lt.p999()), 999'000.0, 50.0);
}

// 10. Memory pool: allocate N, free all, allocate N again -- no crash.
TEST(MemoryPool, AllocFreeReuse) {
  MemoryPool<uint64_t> pool(1000);
  std::vector<uint64_t*> ptrs;
  for (int round = 0; round < 2; ++round) {
    for (int i = 0; i < 1000; ++i) {
      uint64_t* p = pool.allocate(static_cast<uint64_t>(i));
      ASSERT_NE(p, nullptr);
      EXPECT_EQ(*p, static_cast<uint64_t>(i));
      ptrs.push_back(p);
    }
    EXPECT_EQ(pool.in_use(), 1000u);
    for (uint64_t* p : ptrs) {
      pool.deallocate(p);
    }
    ptrs.clear();
    EXPECT_EQ(pool.in_use(), 0u);
  }
}

// 11. Memory pool exhaustion returns nullptr and is counted (no crash).
TEST(MemoryPool, ExhaustionReturnsNull) {
  MemoryPool<int> pool(8);
  for (int i = 0; i < 8; ++i) {
    ASSERT_NE(pool.allocate(i), nullptr);
  }
  EXPECT_EQ(pool.allocate(99), nullptr);
  EXPECT_EQ(pool.exhaustion_count(), 1u);
}

// 12. ist_now_ns() is offset from CLOCK_REALTIME by exactly +5:30.
TEST(IstTime, OffsetFromRealtime) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  const uint64_t utc_ns =
      static_cast<uint64_t>(ts.tv_sec) * kNsPerSecond + static_cast<uint64_t>(ts.tv_nsec);
  const uint64_t ist = ist_now_ns();
  const int64_t diff_s = static_cast<int64_t>((ist - utc_ns) / kNsPerSecond);
  // Within a second of the fixed IST offset.
  EXPECT_NEAR(static_cast<double>(diff_s), static_cast<double>(kIstOffsetSeconds), 1.0);
}

// 13 & 14. Market open / close map to 09:15 and 15:30 IST.
TEST(IstTime, MarketBoundaries) {
  const Timestamp ref = ist_now_ns();
  const Timestamp midnight = ist_midnight_ns(ref);
  EXPECT_EQ(market_open_ns(ref) - midnight, (9ULL * 3600 + 15 * 60) * kNsPerSecond);
  EXPECT_EQ(market_close_ns(ref) - midnight, (15ULL * 3600 + 30 * 60) * kNsPerSecond);
}

// 15 & 16. Pre-market vs in-market classification.
TEST(IstTime, MarketHoursClassification) {
  const Timestamp ref = ist_now_ns();
  const Timestamp t_0914_59 = ist_time_of_day_ns(ref, 9, 14, 59);
  const Timestamp t_0915_01 = ist_time_of_day_ns(ref, 9, 15, 1);
  EXPECT_FALSE(is_within_market_hours(t_0914_59));
  EXPECT_TRUE(is_preopen(t_0914_59));
  EXPECT_TRUE(is_within_market_hours(t_0915_01));
  EXPECT_FALSE(is_preopen(t_0915_01));
}

}  // namespace
}  // namespace hft
