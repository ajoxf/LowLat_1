// Timing utilities: RDTSC clock, IST-aware wall clock, NSE session boundaries,
// and a pre-allocated latency tracker.
//
// All prices are integers and all times are nanoseconds. NSE exchange
// timestamps are in IST (UTC+5:30); we work in IST throughout for logging and
// order timestamps.
#ifndef HFT_COMMON_TIME_UTILS_HPP_
#define HFT_COMMON_TIME_UTILS_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <vector>

#include "common/compiler.hpp"
#include "common/types.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace hft {

// IST is a fixed offset from UTC (no daylight saving): +5 hours 30 minutes.
inline constexpr int64_t kIstOffsetSeconds = 5 * 3600 + 30 * 60;
inline constexpr uint64_t kNsPerSecond = 1'000'000'000ULL;
inline constexpr uint64_t kNsPerDay = 86'400ULL * kNsPerSecond;

// Read the CPU time-stamp counter. This is the cheapest available clock on the
// hot path (a handful of cycles) and is used for all latency measurement.
HFT_ALWAYS_INLINE uint64_t rdtsc() {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  // Portable fallback for non-x86 (e.g. CI on ARM): monotonic nanoseconds.
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * kNsPerSecond +
         static_cast<uint64_t>(ts.tv_nsec);
#endif
}

// Calibrates and caches the TSC frequency (ticks per second) by comparing the
// counter against CLOCK_MONOTONIC over a short interval. Thread-safe, computed
// once on first use.
class TscClock {
 public:
  // Returns the shared, lazily-calibrated instance.
  static const TscClock& Instance() {
    static const TscClock instance;
    return instance;
  }

  double ticks_per_ns() const { return ticks_per_ns_; }
  uint64_t ticks_per_second() const {
    return static_cast<uint64_t>(ticks_per_ns_ * static_cast<double>(kNsPerSecond));
  }

  // Convert a raw TSC delta to nanoseconds.
  HFT_ALWAYS_INLINE uint64_t to_ns(uint64_t cycles) const {
    return static_cast<uint64_t>(static_cast<double>(cycles) / ticks_per_ns_);
  }

 private:
  TscClock() : ticks_per_ns_(Calibrate()) {}

  static double Calibrate() {
#if defined(__x86_64__) || defined(__i386__)
    constexpr uint64_t kCalibrationNs = 100 * 1'000'000ULL;  // 100 ms.
    struct timespec start_ts;
    struct timespec end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    const uint64_t start_tsc = rdtsc();
    uint64_t elapsed_ns = 0;
    do {
      clock_gettime(CLOCK_MONOTONIC, &end_ts);
      elapsed_ns = (static_cast<uint64_t>(end_ts.tv_sec - start_ts.tv_sec) * kNsPerSecond) +
                   static_cast<uint64_t>(end_ts.tv_nsec - start_ts.tv_nsec);
    } while (elapsed_ns < kCalibrationNs);
    const uint64_t end_tsc = rdtsc();
    const double tpns =
        static_cast<double>(end_tsc - start_tsc) / static_cast<double>(elapsed_ns);
    return tpns > 0.0 ? tpns : 1.0;
#else
    // On the fallback clock, rdtsc() already returns nanoseconds.
    return 1.0;
#endif
  }

  double ticks_per_ns_;
};

// Convenience wrapper.
HFT_ALWAYS_INLINE uint64_t rdtsc_to_ns(uint64_t cycles) {
  return TscClock::Instance().to_ns(cycles);
}

// Current wall-clock time in nanoseconds since the Unix epoch, in the IST
// frame (i.e. epoch nanos + IST offset). Used for logging and order stamps.
HFT_ALWAYS_INLINE Timestamp ist_now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  const uint64_t utc_ns =
      static_cast<uint64_t>(ts.tv_sec) * kNsPerSecond + static_cast<uint64_t>(ts.tv_nsec);
  return utc_ns + static_cast<uint64_t>(kIstOffsetSeconds) * kNsPerSecond;
}

// Returns midnight IST (00:00:00) for the IST day that contains ist_ns, as an
// IST-frame nanosecond timestamp.
HFT_ALWAYS_INLINE Timestamp ist_midnight_ns(Timestamp ist_ns) {
  return (ist_ns / kNsPerDay) * kNsPerDay;
}

// Returns the IST-frame nanosecond timestamp for the given hour:minute:second
// on the same IST day as reference_ist_ns.
HFT_ALWAYS_INLINE Timestamp ist_time_of_day_ns(Timestamp reference_ist_ns, int hour,
                                               int minute, int second) {
  const uint64_t tod = (static_cast<uint64_t>(hour) * 3600 +
                        static_cast<uint64_t>(minute) * 60 + static_cast<uint64_t>(second)) *
                       kNsPerSecond;
  return ist_midnight_ns(reference_ist_ns) + tod;
}

// NSE normal market session boundaries for the IST day containing the
// reference timestamp.
HFT_ALWAYS_INLINE Timestamp market_open_ns(Timestamp reference_ist_ns) {
  return ist_time_of_day_ns(reference_ist_ns, 9, 15, 0);  // 09:15:00 IST.
}
HFT_ALWAYS_INLINE Timestamp market_close_ns(Timestamp reference_ist_ns) {
  return ist_time_of_day_ns(reference_ist_ns, 15, 30, 0);  // 15:30:00 IST.
}
HFT_ALWAYS_INLINE Timestamp preopen_start_ns(Timestamp reference_ist_ns) {
  return ist_time_of_day_ns(reference_ist_ns, 9, 0, 0);  // 09:00:00 IST.
}
// 15:25 IST: begin the pre-close cancel sweep.
HFT_ALWAYS_INLINE Timestamp preclose_cancel_ns(Timestamp reference_ist_ns) {
  return ist_time_of_day_ns(reference_ist_ns, 15, 25, 0);
}

// True when the timestamp falls inside the NSE normal-market window.
HFT_ALWAYS_INLINE bool is_within_market_hours(Timestamp ist_ns) {
  return ist_ns >= market_open_ns(ist_ns) && ist_ns < market_close_ns(ist_ns);
}

// True during the pre-open collection window (09:00-09:15 IST).
HFT_ALWAYS_INLINE bool is_preopen(Timestamp ist_ns) {
  return ist_ns >= preopen_start_ns(ist_ns) && ist_ns < market_open_ns(ist_ns);
}

// ---------------------------------------------------------------------------
// LatencyTracker
// ---------------------------------------------------------------------------

// Records latency samples into a pre-allocated buffer (no heap traffic while
// recording) and computes summary statistics on demand.
class LatencyTracker {
 public:
  explicit LatencyTracker(size_t max_samples = 1'000'000)
      : samples_(max_samples, 0),
        count_(0),
        min_(UINT64_MAX),
        max_(0),
        sum_(0) {}

  // Record one latency sample (in nanoseconds). O(1), never allocates. Once
  // the pre-allocated buffer is full, summary stats (min/max/mean) keep
  // updating but percentiles are computed over the retained samples.
  HFT_ALWAYS_INLINE void record(uint64_t ns) {
    if (HFT_LIKELY(count_ < samples_.size())) {
      samples_[count_] = ns;
    }
    ++count_;
    min_ = std::min(min_, ns);
    max_ = std::max(max_, ns);
    sum_ += ns;
  }

  uint64_t count() const { return count_; }
  uint64_t min() const { return count_ == 0 ? 0 : min_; }
  uint64_t max() const { return max_; }
  uint64_t mean() const { return count_ == 0 ? 0 : sum_ / count_; }

  uint64_t percentile(double p) const {
    const size_t n = retained();
    if (n == 0) {
      return 0;
    }
    std::vector<uint64_t> sorted(samples_.begin(), samples_.begin() + static_cast<long>(n));
    std::sort(sorted.begin(), sorted.end());
    double rank = p / 100.0 * static_cast<double>(n - 1);
    size_t idx = static_cast<size_t>(rank + 0.5);
    if (idx >= n) {
      idx = n - 1;
    }
    return sorted[idx];
  }

  uint64_t p50() const { return percentile(50.0); }
  uint64_t p99() const { return percentile(99.0); }
  uint64_t p999() const { return percentile(99.9); }

  void reset() {
    count_ = 0;
    min_ = UINT64_MAX;
    max_ = 0;
    sum_ = 0;
  }

 private:
  size_t retained() const {
    return static_cast<size_t>(std::min<uint64_t>(count_, samples_.size()));
  }

  std::vector<uint64_t> samples_;  // Pre-allocated once; never grows.
  uint64_t count_;
  uint64_t min_;
  uint64_t max_;
  uint64_t sum_;
};

}  // namespace hft

#endif  // HFT_COMMON_TIME_UTILS_HPP_
