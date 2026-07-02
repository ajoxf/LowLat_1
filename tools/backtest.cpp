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
#include "sim/fee_model.hpp"

namespace {

constexpr hft::Token kToken = 2885;
constexpr hft::Price kMid = 1000000;
constexpr hft::Price kTick = 5;

void PrintRow(const char* name, const hft::BtResult& r) {
  std::printf("%-12s | %9lld | %8lld | %9lld | %7llu | %5lld\n", name,
              static_cast<long long>(r.gross_pnl_inr), static_cast<long long>(r.fees_inr),
              static_cast<long long>(r.pnl_inr), static_cast<unsigned long long>(r.fills),
              static_cast<long long>(r.position));
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

  // Realistic NSE FO cost model (both legs charged, STT on sells, no rebate).
  hft::FeeModel fees;  // defaults: rough NSE FO futures rates.

  // The LES / designated-market-maker lever: the same costs plus a maker rebate
  // credited on both legs. This is the ONLY place a real NSE rebate exists.
  hft::FeeModel fees_les = fees;
  fees_les.maker_rebate_bp100 = 250;  // 2.5 bp rebate per leg (illustrative LES).

  const hft::BtResult rn =
      hft::run_backtest(events, naive, kToken, kMid, kTick, nullptr, fees);
  const hft::BtResult rs =
      hft::run_backtest(events, smart, kToken, kMid, kTick, "backtest_metrics.jsonl", fees);
  const hft::BtResult rl =
      hft::run_backtest(events, smart, kToken, kMid, kTick, nullptr, fees_les);

  std::printf("\n=== backtest comparison (same data, %zu events) ===\n", events.size());
  std::printf("variant      |  gross Rs |  fees Rs |    net Rs |  fills  | pos\n");
  std::printf("-------------+-----------+----------+-----------+---------+------\n");
  PrintRow("naive", rn);
  PrintRow("smart", rs);
  PrintRow("smart+LES", rl);
  std::printf("-------------+-----------+----------+-----------+---------+------\n");
  std::printf("gross->net drag from NSE costs (smart): Rs %lld\n",
              static_cast<long long>(rs.gross_pnl_inr - rs.pnl_inr));
  std::printf("LES rebate lever (smart+LES net - smart net): Rs %lld\n",
              static_cast<long long>(rl.pnl_inr - rs.pnl_inr));

  std::printf("\nNOTE: queue-aware fills + NSE cost model + synthetic random-walk data.\n");
  std::printf("Costs/rebates are ROUGH defaults -- set them from your member rate card.\n");
  std::printf("Random walk has no edge; use recorded NSE MTBT for real PnL.\n");
  std::printf("Visualise: serve the repo and open dashboard/index.html (smart -> backtest_metrics.jsonl)\n");
  return 0;
}
