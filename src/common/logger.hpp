// Asynchronous logger.
//
// The hot path only ever writes a fixed-size POD entry into a lock-free ring
// buffer -- it never formats strings, never calls write()/fprintf, and never
// blocks. A background thread drains the ring buffer and renders entries to
// disk. CRITICAL entries trigger an immediate flush.
#ifndef HFT_COMMON_LOGGER_HPP_
#define HFT_COMMON_LOGGER_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "common/compiler.hpp"
#include "common/ring_buffer.hpp"
#include "common/time_utils.hpp"

namespace hft {

enum class LogLevel : uint8_t {
  kDebug = 0,
  kInfo = 1,
  kWarn = 2,
  kError = 3,
  kCritical = 4,
};

inline const char* to_string(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kError:
      return "ERROR";
    case LogLevel::kCritical:
      return "CRITICAL";
  }
  return "?";
}

// Fixed-size log record. No std::string -- the hot path copies a short message
// tag plus up to four numeric arguments. Rendering happens on the drain thread.
struct LogEntry {
  Timestamp ts_ist;
  LogLevel level;
  char tag[31];      // Null-terminated short message (no formatting on hot path).
  int64_t arg0;
  int64_t arg1;
  int64_t arg2;
  int64_t arg3;
};

// Single global async logger. Capacity is a compile-time power of two.
template <size_t Capacity = 1U << 16>
class AsyncLoggerT {
 public:
  explicit AsyncLoggerT(const char* path = nullptr)
      : file_(path != nullptr ? std::fopen(path, "w") : stderr),
        owns_file_(path != nullptr),
        running_(true),
        dropped_(0) {
    drain_thread_ = std::thread([this] { DrainLoop(); });
  }

  ~AsyncLoggerT() {
    running_.store(false, std::memory_order_release);
    if (drain_thread_.joinable()) {
      drain_thread_.join();
    }
    Flush();
    if (owns_file_ && file_ != nullptr) {
      std::fclose(file_);
    }
  }

  AsyncLoggerT(const AsyncLoggerT&) = delete;
  AsyncLoggerT& operator=(const AsyncLoggerT&) = delete;

  // Hot-path entry point. Never blocks, never formats. Returns false if the
  // ring buffer was full (entry dropped, counter bumped).
  HFT_ALWAYS_INLINE bool log(LogLevel level, const char* tag, int64_t a0 = 0, int64_t a1 = 0,
                             int64_t a2 = 0, int64_t a3 = 0) {
    LogEntry e;
    e.ts_ist = ist_now_ns();
    e.level = level;
    std::strncpy(e.tag, tag, sizeof(e.tag) - 1);
    e.tag[sizeof(e.tag) - 1] = '\0';
    e.arg0 = a0;
    e.arg1 = a1;
    e.arg2 = a2;
    e.arg3 = a3;
    if (HFT_UNLIKELY(!queue_.push(e))) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    // CRITICAL: ask the drain thread to flush immediately.
    if (HFT_UNLIKELY(level == LogLevel::kCritical)) {
      flush_request_.store(true, std::memory_order_release);
    }
    return true;
  }

  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  // Drain everything currently queued (used by tests and on shutdown).
  void Flush() {
    LogEntry e;
    while (queue_.pop(e)) {
      Render(e);
    }
    if (file_ != nullptr) {
      std::fflush(file_);
    }
  }

 private:
  void DrainLoop() {
    LogEntry e;
    while (running_.load(std::memory_order_acquire)) {
      bool any = false;
      while (queue_.pop(e)) {
        Render(e);
        any = true;
      }
      if (flush_request_.exchange(false, std::memory_order_acq_rel) || any) {
        if (file_ != nullptr) {
          std::fflush(file_);
        }
      }
      if (!any) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    }
  }

  void Render(const LogEntry& e) {
    if (file_ == nullptr) {
      return;
    }
    std::fprintf(file_, "%llu [%s] %s %lld %lld %lld %lld\n",
                 static_cast<unsigned long long>(e.ts_ist), to_string(e.level), e.tag,
                 static_cast<long long>(e.arg0), static_cast<long long>(e.arg1),
                 static_cast<long long>(e.arg2), static_cast<long long>(e.arg3));
  }

  RingBuffer<LogEntry, Capacity> queue_;
  std::FILE* file_;
  bool owns_file_;
  std::atomic<bool> running_;
  std::atomic<bool> flush_request_{false};
  std::atomic<uint64_t> dropped_;
  std::thread drain_thread_;
};

using AsyncLogger = AsyncLoggerT<>;

}  // namespace hft

#endif  // HFT_COMMON_LOGGER_HPP_
