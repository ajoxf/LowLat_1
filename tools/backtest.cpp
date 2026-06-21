// backtest: replay MTBT through the full pipeline with a queue-aware fill
// simulator and report simulated PnL plus tick-to-trade latency. Runs the naive
// and smart (skew-on) strategy on identical data and reports the delta.
//
//   ./backtest [capture.mtbt]
//
// With no file it generates a synthetic depth-bearing MBO session in memory. The
// fill model accounts for queue position; it is realistic enough to compare
// variants but still synthetic data -- trust PnL only on recorded NSE MTBT.
#include <cstdio>
#include <vector>

#include "common/types.hpp"
#include "sim/backtest_engine.hpp"
#include "sim/event_source.hpp"

namespace {

constexpr hft::Token kToken = 2885;
constexpr hft::Price kMid = 1000000;
constexpr hft::Price kTick = 5;

void PrintRow(const char* name, const hft::BtResult& r) {
  std::printf("%-8s | %9lld | %7llu | %6llu | %5lld | p99=%llu ns\n", name,
              static_cast<long long>(r.pnl_inr), static_cast<unsigned long long>(r.fills),
              static_cast<unsigned long long>(r.orders_sent),
              static_cast<long long>(r.position), static_cast<unsigned long long>(r.lat_p99));
}

}  // namespace

int main(int argc, char** argv) {
  const char* capture = argc >= 2 ? argv[1] : nullptr;
  std::vector<hft::MarketEvent> events = hft::load_events(capture, kToken, kMid, kTick, 200000);
  std::printf("backtest: %zu events (%s)\n", events.size(),
              capture != nullptr ? capture : "synthetic MBO");

  hft::MarketMakingConfig naive;
  naive.max_position_lots = 50;

  hft::MarketMakingConfig smart = naive;
  smart.max_inv_skew_ticks = 2;
  smart.max_flow_skew_ticks = 1;

  const hft::BtResult rn =
      hft::run_backtest(events, naive, kToken, kMid, kTick, "backtest_metrics_naive.jsonl");
  const hft::BtResult rs =
      hft::run_backtest(events, smart, kToken, kMid, kTick, "backtest_metrics.jsonl");

  std::printf("\n=== backtest comparison (same data, %zu events) ===\n", events.size());
  std::printf("variant  | PnL (Rs) |  fills  | orders | pos   | latency\n");
  std::printf("---------+-----------+---------+--------+-------+-----------\n");
  PrintRow("naive", rn);
  PrintRow("smart", rs);
  std::printf("---------+-----------+---------+--------+-------+-----------\n");
  std::printf("PnL improvement (smart - naive): Rs %lld\n",
              static_cast<long long>(rs.pnl_inr - rn.pnl_inr));

  std::printf("\nNOTE: queue-aware fill model + synthetic random-walk data.\n");
  std::printf("Random walk has no edge to capture; use recorded NSE MTBT for real PnL.\n");
  std::printf("Visualise: serve the repo and open dashboard/index.html\n");
  std::printf("  smart -> backtest_metrics.jsonl   naive -> backtest_metrics_naive.jsonl\n");
  return 0;
}
