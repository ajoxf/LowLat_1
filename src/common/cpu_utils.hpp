// CPU / OS tuning helpers: thread pinning, real-time scheduling, huge pages,
// and prefetch. These are best-effort: on a developer box (or in CI) the calls
// may be denied for lack of privilege, which is reported via the return value
// rather than aborting.
#ifndef HFT_COMMON_CPU_UTILS_HPP_
#define HFT_COMMON_CPU_UTILS_HPP_

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>

#include "common/compiler.hpp"

namespace hft {

// Pin the calling thread to a single physical core. Returns true on success.
inline bool pin_thread_to_core(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}

// Request SCHED_FIFO real-time scheduling at the given priority (1..99).
// Requires CAP_SYS_NICE; returns false if denied.
inline bool set_thread_realtime(int priority) {
  struct sched_param param;
  param.sched_priority = priority;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
}

// Allocate huge-page-backed memory (2 MB pages). Falls back to nullptr if the
// kernel cannot satisfy the request. Use munmap_huge_pages() to release.
inline void* allocate_huge_pages(size_t bytes) {
  void* ptr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  if (ptr == MAP_FAILED) {
    // Retry without huge pages so callers still get usable memory off the
    // critical path (e.g. on machines without huge pages configured).
    ptr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
      return nullptr;
    }
  }
  return ptr;
}

inline void free_huge_pages(void* ptr, size_t bytes) {
  if (ptr != nullptr) {
    munmap(ptr, bytes);
  }
}

// Software prefetch hint for the hot path.
HFT_ALWAYS_INLINE void prefetch(const void* addr) { __builtin_prefetch(addr); }

}  // namespace hft

#endif  // HFT_COMMON_CPU_UTILS_HPP_
