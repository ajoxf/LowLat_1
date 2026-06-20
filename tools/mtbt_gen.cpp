// mtbt_gen: write a synthetic NSE MTBT capture file for offline backtesting.
//
// Output is the length-prefixed format read by mtbt_replay and backtest:
// repeated [u32-le length][packet bytes]. Each packet carries one MTBT message.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "common/types.hpp"
#include "sim/mtbt_generator.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <out.mtbt> [count=100000] [token=2885] [mid_paisa=1000000] "
                 "[tick=5]\n",
                 argv[0]);
    return 2;
  }
  const char* out_path = argv[1];
  const int count = argc >= 3 ? std::atoi(argv[2]) : 100000;
  const auto token = static_cast<hft::Token>(argc >= 4 ? std::strtoul(argv[3], nullptr, 10)
                                                       : 2885);
  const auto mid = static_cast<hft::Price>(argc >= 5 ? std::strtoll(argv[4], nullptr, 10)
                                                     : 1000000);
  const auto tick = static_cast<hft::Price>(argc >= 6 ? std::strtoll(argv[5], nullptr, 10) : 5);

  hft::MtbtGenerator gen(token, mid, tick, /*seed=*/12345);
  std::vector<hft::MarketEvent> events;
  gen.generate(events, count);

  std::vector<uint8_t> bytes;
  for (const auto& ev : events) {
    hft::MtbtGenerator::encode_capture(ev, bytes);
  }

  std::FILE* f = std::fopen(out_path, "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "error: cannot open %s for writing\n", out_path);
    return 1;
  }
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);

  std::printf("mtbt_gen: wrote %zu events (%zu bytes) for token %u to %s\n", events.size(),
              bytes.size(), token, out_path);
  return 0;
}
