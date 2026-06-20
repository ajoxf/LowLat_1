// Milestone 1 benchmark: ring buffer throughput and one-way latency.
#include "common/ring_buffer.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <thread>

#include "common/time_utils.hpp"

namespace {

// Single-threaded push+pop throughput (steady state).
void BM_RingBufferRoundtrip(benchmark::State& state) {
  hft::RingBuffer<uint64_t, 1024> rb;
  uint64_t v = 0;
  uint64_t out = 0;
  for (auto _ : state) {
    rb.push(v++);
    rb.pop(out);
    benchmark::DoNotOptimize(out);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBufferRoundtrip);

// Cross-thread sustained throughput.
void BM_RingBufferThroughput(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    hft::RingBuffer<uint64_t, 4096> rb;
    constexpr uint64_t kCount = 4'000'000;
    std::atomic<bool> go{false};
    std::thread prod([&] {
      while (!go.load()) {
      }
      for (uint64_t i = 0; i < kCount; ++i) {
        while (!rb.push(i)) {
        }
      }
    });
    state.ResumeTiming();
    go.store(true);
    uint64_t out = 0;
    uint64_t n = 0;
    while (n < kCount) {
      if (rb.pop(out)) {
        ++n;
      }
    }
    prod.join();
    state.SetItemsProcessed(state.items_processed() + static_cast<int64_t>(kCount));
  }
}
BENCHMARK(BM_RingBufferThroughput)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
