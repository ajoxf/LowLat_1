// backtest: replay MTBT through the full pipeline with a fill simulator and
// report simulated PnL plus tick-to-trade latency.
//
//   ./backtest [capture.mtbt]
//
// With no file it generates a synthetic random-walk session in memory. The fill
// model is the optimistic FillSimulator (front-of-queue at our price); use it
// to compare strategy variants and to sanity-check latency, NOT as a P&L
// guarantee. Real validation needs recorded NSE MTBT and a queue-aware model.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "common/time_utils.hpp"
#include "feed_handler/mtbt_parser.hpp"
#include "monitor/metrics.hpp"
#include "order_book/book_manager.hpp"
#include "order_manager/order_manager.hpp"
#include "pipeline.hpp"
#include "risk/risk_manager.hpp"
#include "session/symbol_master.hpp"
#include "sim/fill_simulator.hpp"
#include "sim/mtbt_generator.hpp"
#include "strategy/market_making.hpp"

namespace {

constexpr hft::Token kToken = 2885;
constexpr hft::Price kMid = 1000000;
constexpr hft::Price kTick = 5;

bool ReadU32(std::FILE* f, uint32_t* out) {
  uint8_t b[4];
  if (std::fread(b, 1, 4, f) != 4) {
    return false;
  }
  *out = b[0] | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
  return true;
}

// Load events from a capture file, or generate a synthetic session.
std::vector<hft::MarketEvent> LoadEvents(const char* path) {
  std::vector<hft::MarketEvent> events;
  if (path == nullptr) {
    hft::MtbtGenerator gen(kToken, kMid, kTick, 12345);
    gen.generate(events, 200000);
    std::printf("backtest: generated %zu synthetic events for token %u\n", events.size(),
                kToken);
    return events;
  }
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "error: cannot open %s\n", path);
    return events;
  }
  hft::MtbtParser parser;
  std::vector<uint8_t> buf;
  uint32_t len = 0;
  while (ReadU32(f, &len)) {
    if (len == 0 || len > (1U << 20)) {
      break;
    }
    buf.resize(len);
    if (std::fread(buf.data(), 1, len, f) != len) {
      break;
    }
    parser.parse_packet(buf.data(), len, [&](const hft::MarketEvent& e) { events.push_back(e); });
  }
  std::fclose(f);
  std::printf("backtest: loaded %zu events from %s\n", events.size(), path);
  return events;
}

}  // namespace

namespace {

struct BtResult {
  uint64_t orders_sent = 0;
  uint64_t orders_rejected = 0;
  uint64_t fills = 0;
  int64_t position = 0;
  int64_t pnl_inr = 0;
  uint64_t lat_p50 = 0;
  uint64_t lat_p99 = 0;
  uint64_t lat_p999 = 0;
};

// Run the full pipeline + fill simulator over `events` with the given strategy
// config. Optionally writes a dashboard metrics file.
BtResult RunBacktest(const std::vector<hft::MarketEvent>& events,
                     const hft::MarketMakingConfig& mm_cfg, const char* metrics_path) {
  hft::SymbolMaster symbols;
  hft::Instrument cm{};
  cm.token = kToken;
  cm.segment = hft::kSegmentCm;
  cm.instrument_type = hft::InstrumentType::kEquity;
  cm.lot_size = 1;
  cm.tick_size = kTick;
  cm.prev_close = kMid;
  cm.circuit_pct_x100 = 2000;  // 20% band.
  symbols.put(cm);

  hft::BookManager books;
  books.add_token(kToken, kTick);
  hft::MarketMaking strat(&symbols, mm_cfg);

  hft::RiskConfig risk_cfg;
  risk_cfg.max_orders_per_sec = 1'000'000'000;  // Do not throttle the backtest.
  risk_cfg.max_token_position_lots = 1'000'000;
  risk_cfg.price_sanity_pct = 50;
  hft::RiskManager risk(&symbols, risk_cfg);

  hft::OrderManager om;
  hft::Pipeline pipe(&books, &strat, &risk, &om);
  pipe.set_now(hft::ist_time_of_day_ns(hft::ist_now_ns(), 10, 0, 0));

  hft::FillSimulator fillsim;
  hft::MetricsWriter metrics;
  const bool want_metrics = metrics_path != nullptr && metrics.open(metrics_path);
  const uint64_t snapshot_every = events.empty() ? 1 : (events.size() / 200) + 1;

  int64_t cash_paisa = 0;
  int64_t position = 0;
  hft::Price mark = kMid;
  uint64_t maker_fills = 0;
  uint64_t event_index = 0;

  for (const hft::MarketEvent& ev : events) {
    if (static_cast<hft::MtbtMsgType>(ev.type) == hft::MtbtMsgType::kSnapQuote) {
      mark = (ev.bid_price + ev.ask_price) / 2;
    }
    fillsim.on_event(ev, [&](const hft::FillReport& f) {
      pipe.on_fill(f);
      if (f.side == hft::kBuy) {
        cash_paisa -= static_cast<int64_t>(f.fill_qty) * f.fill_price;
        position += f.fill_qty;
      } else {
        cash_paisa += static_cast<int64_t>(f.fill_qty) * f.fill_price;
        position -= f.fill_qty;
      }
      ++maker_fills;
    });
    pipe.process(ev);
    for (const hft::Order& o : pipe.last_sent()) {
      fillsim.register_order(o);
    }
    ++event_index;
    if (want_metrics && event_index % snapshot_every == 0) {
      hft::MetricSnapshot snap;
      snap.t = event_index;
      snap.events = pipe.stats().events;
      snap.orders_sent = pipe.stats().orders_sent;
      snap.orders_rejected = pipe.stats().orders_rejected;
      snap.fills = maker_fills;
      snap.position = position;
      snap.pnl_inr = (cash_paisa + position * mark) / 100;
      snap.lat_p50 = pipe.latency().p50();
      snap.lat_p99 = pipe.latency().p99();
      snap.lat_p999 = pipe.latency().p999();
      metrics.write(snap);
    }
  }
  metrics.flush();

  BtResult r;
  r.orders_sent = pipe.stats().orders_sent;
  r.orders_rejected = pipe.stats().orders_rejected;
  r.fills = maker_fills;
  r.position = position;
  r.pnl_inr = (cash_paisa + position * mark) / 100;
  r.lat_p50 = pipe.latency().p50();
  r.lat_p99 = pipe.latency().p99();
  r.lat_p999 = pipe.latency().p999();
  return r;
}

void PrintRow(const char* name, const BtResult& r) {
  std::printf("%-8s | %9lld | %7llu | %6llu | %5lld | p99=%llu ns\n", name,
              static_cast<long long>(r.pnl_inr), static_cast<unsigned long long>(r.fills),
              static_cast<unsigned long long>(r.orders_sent),
              static_cast<long long>(r.position), static_cast<unsigned long long>(r.lat_p99));
}

}  // namespace

int main(int argc, char** argv) {
  const char* capture = argc >= 2 ? argv[1] : nullptr;

  std::vector<hft::MarketEvent> events = LoadEvents(capture);

  // Naive: one tick inside the touch, no skew.
  hft::MarketMakingConfig naive;
  naive.max_position_lots = 50;

  // Smart: same, plus inventory-skew and order-flow-imbalance quoting.
  hft::MarketMakingConfig smart = naive;
  smart.max_inv_skew_ticks = 2;
  smart.max_flow_skew_ticks = 1;

  const BtResult rn = RunBacktest(events, naive, "backtest_metrics_naive.jsonl");
  const BtResult rs = RunBacktest(events, smart, "backtest_metrics.jsonl");

  std::printf("\n=== backtest comparison (same data, %zu events) ===\n", events.size());
  std::printf("variant  | PnL (Rs) |  fills  | orders | pos   | latency\n");
  std::printf("---------+-----------+---------+--------+-------+-----------\n");
  PrintRow("naive", rn);
  PrintRow("smart", rs);
  const int64_t delta = rs.pnl_inr - rn.pnl_inr;
  std::printf("---------+-----------+---------+--------+-------+-----------\n");
  std::printf("PnL improvement (smart - naive): Rs %lld\n", static_cast<long long>(delta));

  std::printf("\nNOTE: optimistic front-of-queue fill model + synthetic data.\n");
  std::printf("Use recorded NSE MTBT and a queue-aware model before trusting PnL.\n");
  std::printf("Visualise: serve the repo and open dashboard/index.html\n");
  std::printf("  smart -> backtest_metrics.jsonl   naive -> backtest_metrics_naive.jsonl\n");
  return 0;
}
