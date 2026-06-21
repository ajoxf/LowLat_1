// sweep: grid-search the market-maker's skew parameters over a single dataset
// and report a PnL heatmap, so you can find the settings that perform best
// (or, on a featureless random walk, bleed the least).
//
//   ./sweep [capture.mtbt]
//
// Sweeps max_inv_skew_ticks (rows) x max_flow_skew_ticks (cols). Writes
// sweep_results.csv. NOTE: tuning on synthetic random-walk data overfits to
// noise -- run this on RECORDED NSE MTBT for a meaningful result.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/types.hpp"
#include "sim/backtest_engine.hpp"
#include "sim/event_source.hpp"

namespace {

constexpr hft::Token kToken = 2885;
constexpr hft::Price kMid = 1000000;
constexpr hft::Price kTick = 5;
constexpr int kMaxInv = 4;   // rows: max_inv_skew_ticks 0..4
constexpr int kMaxFlow = 3;  // cols: max_flow_skew_ticks 0..3

}  // namespace

int main(int argc, char** argv) {
  const char* capture = argc >= 2 ? argv[1] : nullptr;
  std::vector<hft::MarketEvent> events = hft::load_events(capture, kToken, kMid, kTick, 200000);
  std::printf("sweep: %zu events (%s)\n", events.size(),
              capture != nullptr ? capture : "synthetic MBO");

  int64_t pnl[kMaxInv + 1][kMaxFlow + 1];
  int best_inv = 0, best_flow = 0;
  int64_t best_pnl = INT64_MIN;

  std::FILE* csv = std::fopen("sweep_results.csv", "w");
  if (csv != nullptr) {
    std::fprintf(csv, "inv_skew_ticks,flow_skew_ticks,pnl_inr,fills,orders,position\n");
  }

  for (int inv = 0; inv <= kMaxInv; ++inv) {
    for (int flow = 0; flow <= kMaxFlow; ++flow) {
      hft::MarketMakingConfig cfg;
      cfg.max_position_lots = 50;
      cfg.max_inv_skew_ticks = inv;
      cfg.max_flow_skew_ticks = flow;
      const hft::BtResult r =
          hft::run_backtest(events, cfg, kToken, kMid, kTick, nullptr);
      pnl[inv][flow] = r.pnl_inr;
      if (r.pnl_inr > best_pnl) {
        best_pnl = r.pnl_inr;
        best_inv = inv;
        best_flow = flow;
      }
      if (csv != nullptr) {
        std::fprintf(csv, "%d,%d,%lld,%llu,%llu,%lld\n", inv, flow,
                     static_cast<long long>(r.pnl_inr),
                     static_cast<unsigned long long>(r.fills),
                     static_cast<unsigned long long>(r.orders_sent),
                     static_cast<long long>(r.position));
      }
    }
  }
  if (csv != nullptr) {
    std::fclose(csv);
  }

  // PnL heatmap (rows = inventory skew, cols = order-flow skew).
  std::printf("\nPnL (Rs) heatmap -- rows: inv_skew_ticks, cols: flow_skew_ticks\n");
  std::printf("        ");
  for (int flow = 0; flow <= kMaxFlow; ++flow) {
    std::printf("flow=%-1d   ", flow);
  }
  std::printf("\n");
  for (int inv = 0; inv <= kMaxInv; ++inv) {
    std::printf("inv=%-1d | ", inv);
    for (int flow = 0; flow <= kMaxFlow; ++flow) {
      const char* star = (inv == best_inv && flow == best_flow) ? "*" : " ";
      std::printf("%7lld%s", static_cast<long long>(pnl[inv][flow]), star);
    }
    std::printf("\n");
  }

  std::printf(
      "\nbest: inv_skew=%d flow_skew=%d -> Rs %lld   (naive inv=0,flow=0 -> Rs %lld)\n",
      best_inv, best_flow, static_cast<long long>(best_pnl),
      static_cast<long long>(pnl[0][0]));
  std::printf("results -> sweep_results.csv\n");
  std::printf("\nNOTE: synthetic random walk has no edge; this finds the least-bad\n");
  std::printf("settings. Re-run on recorded NSE MTBT for a tuning you can trust.\n");
  return 0;
}
