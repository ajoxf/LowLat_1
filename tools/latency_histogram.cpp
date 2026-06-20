// latency_histogram: render an ASCII latency histogram with p50/p99/p999.
//
// Reads newline-separated latency samples in nanoseconds from a file (or stdin
// when given "-"). With no argument it renders a synthetic distribution so the
// output format can be inspected. In production the monitoring thread (Core 7)
// feeds live samples from a shared-memory ring buffer into the same renderer.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "common/time_utils.hpp"

namespace {

void Render(hft::LatencyTracker& lt, uint64_t max_ns) {
  constexpr int kBuckets = 50;
  // Recompute a simple histogram from the tracker's percentiles is not enough;
  // we re-derive buckets by sampling percentiles for a readable shape.
  std::printf("\nLatency histogram (n=%llu, range 0..%lluns)\n",
              static_cast<unsigned long long>(lt.count()),
              static_cast<unsigned long long>(max_ns));
  for (int i = 1; i <= kBuckets; ++i) {
    const double p = 100.0 * i / kBuckets;
    const uint64_t v = lt.percentile(p);
    const int bar = max_ns > 0 ? static_cast<int>(60.0 * v / max_ns) : 0;
    std::printf("p%5.1f %8lluns |", p, static_cast<unsigned long long>(v));
    for (int b = 0; b < bar; ++b) {
      std::putchar('#');
    }
    std::putchar('\n');
  }
  std::printf("\nmin=%lluns p50=%lluns p99=%lluns p999=%lluns max=%lluns mean=%lluns\n",
              static_cast<unsigned long long>(lt.min()),
              static_cast<unsigned long long>(lt.p50()),
              static_cast<unsigned long long>(lt.p99()),
              static_cast<unsigned long long>(lt.p999()),
              static_cast<unsigned long long>(lt.max()),
              static_cast<unsigned long long>(lt.mean()));
}

}  // namespace

int main(int argc, char** argv) {
  hft::LatencyTracker lt(2'000'000);
  uint64_t max_ns = 0;

  if (argc >= 2) {
    std::FILE* f = (argv[1][0] == '-' && argv[1][1] == '\0') ? stdin : std::fopen(argv[1], "r");
    if (f == nullptr) {
      std::fprintf(stderr, "error: cannot open %s\n", argv[1]);
      return 1;
    }
    unsigned long long v = 0;
    while (std::fscanf(f, "%llu", &v) == 1) {
      lt.record(v);
      if (v > max_ns) {
        max_ns = v;
      }
    }
    if (f != stdin) {
      std::fclose(f);
    }
  } else {
    // Synthetic demo: a tick-to-trade-like distribution centred near 2us.
    std::mt19937_64 rng(42);
    std::lognormal_distribution<double> dist(7.6, 0.5);  // ~2000ns median.
    for (int i = 0; i < 100000; ++i) {
      auto v = static_cast<uint64_t>(dist(rng));
      lt.record(v);
      if (v > max_ns) {
        max_ns = v;
      }
    }
    std::printf("(no input file given -- rendering a synthetic demo distribution)\n");
  }

  Render(lt, max_ns);
  return 0;
}
