// symbol_master_loader: load and summarise the daily NSE symbol master CSV.
//
// NSE publishes the symbol master via the member/colocation portal at
// end-of-day; download it there and point this tool at the CSV. The expected
// columns are documented in src/session/symbol_master.hpp.
#include <cstdio>
#include <cstdlib>

#include "session/symbol_master.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <nse_symbols.csv> [token]\n", argv[0]);
    return 2;
  }
  hft::SymbolMaster sm;
  long rows = sm.load_csv(argv[1]);
  if (rows < 0) {
    std::fprintf(stderr, "error: cannot open %s\n", argv[1]);
    return 1;
  }
  std::printf("symbol_master: loaded %ld rows (%zu active)\n", rows, sm.count());

  if (argc >= 3) {
    auto token = static_cast<hft::Token>(std::strtoul(argv[2], nullptr, 10));
    const hft::Instrument* inst = sm.get(token);
    if (inst == nullptr) {
      std::printf("  token %u: not found\n", token);
    } else {
      std::printf("  token %u: %s/%s lot=%d tick=%lld prev_close=%lld circuit=%u%%%s\n",
                  token, inst->symbol, inst->series, inst->lot_size,
                  static_cast<long long>(inst->tick_size),
                  static_cast<long long>(inst->prev_close), inst->circuit_pct_x100 / 100,
                  sm.is_index_derivative(token) ? " [index-deriv]" : "");
    }
  }
  return 0;
}
