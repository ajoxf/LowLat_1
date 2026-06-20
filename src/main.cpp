// NSE HFT system entry point.
//
// Wires the full tick-to-trade pipeline together following the Milestone 8
// startup sequence. Without live NSE colocation connectivity (multicast feed +
// NNF gateway) this binary runs a self-contained synthetic session that
// exercises every stage and reports tick-to-trade latency -- useful as a smoke
// test and as a template for the production wiring.
//
// In production, replace the synthetic event loop with:
//   * MarketDataGateway on a pinned core feeding the event ring buffer, and
//   * OrderGateway / NnfSession driving the NNF order connection.
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "common/time_utils.hpp"
#include "order_book/book_manager.hpp"
#include "order_manager/order_manager.hpp"
#include "pipeline.hpp"
#include "risk/risk_manager.hpp"
#include "session/symbol_master.hpp"
#include "strategy/market_making.hpp"

namespace {

std::atomic<bool> g_running{true};
void HandleSignal(int) { g_running.store(false, std::memory_order_release); }

hft::MarketEvent SnapQuote(hft::Token token, hft::Price bid, hft::Price ask, uint64_t seq) {
  hft::MarketEvent ev{};
  ev.type = static_cast<uint8_t>(hft::MtbtMsgType::kSnapQuote);
  ev.token = token;
  ev.bid_price = bid;
  ev.bid_qty = 100;
  ev.ask_price = ask;
  ev.ask_qty = 100;
  ev.seq_no = seq;
  return ev;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGTERM, HandleSignal);
  std::signal(SIGINT, HandleSignal);

  const char* cm_csv = argc >= 2 ? argv[1] : "config/nse_cm_symbols.csv";
  const hft::Token kToken = 2885;  // RELIANCE EQ.

  std::printf("=== NSE HFT system :: startup ===\n");
  std::printf("[1] TSC calibrated at %.3f GHz\n",
              hft::TscClock::Instance().ticks_per_second() / 1e9);

  // [2] Load the NSE symbol master (fall back to a built-in instrument).
  hft::SymbolMaster symbols;
  long rows = symbols.load_csv(cm_csv);
  if (rows < 0) {
    std::printf("[2] symbol master '%s' not found -- using built-in RELIANCE EQ\n", cm_csv);
    hft::Instrument cm{};
    cm.token = kToken;
    cm.segment = hft::kSegmentCm;
    cm.instrument_type = hft::InstrumentType::kEquity;
    cm.lot_size = 1;
    cm.tick_size = 5;
    cm.prev_close = 1000000;
    cm.circuit_pct_x100 = 2000;
    std::memcpy(cm.symbol, "RELIANCE", 9);
    std::memcpy(cm.series, "EQ", 3);
    symbols.put(cm);
  } else {
    std::printf("[2] symbol master loaded: %ld rows\n", rows);
  }

  // [3] Pre-allocate books, strategy, risk, order manager.
  hft::BookManager books;
  books.add_token(kToken, symbols.get_tick_size(kToken));

  hft::MarketMakingConfig mm_cfg;
  mm_cfg.max_position_lots = 25;
  hft::MarketMaking strat(&symbols, mm_cfg);

  hft::RiskConfig risk_cfg;
  risk_cfg.max_orders_per_sec = 1000;     // NSE subscribed message rate.
  risk_cfg.max_daily_loss_inr = 1'000'000;
  risk_cfg.price_sanity_pct = 50;
  hft::RiskManager risk(&symbols, risk_cfg);

  hft::OrderManager om;
  om.encoder().set_alpha_char('C', 'M');
  om.encoder().set_trader_id(0);

  hft::Pipeline pipe(&books, &strat, &risk, &om);
  std::printf("[3] pipeline allocated (1 watched token)\n");

  // [4] Pin threads / set RT priority would go here on colo hardware.
  // [5] NNF login and [6] MTBT subscribe are skipped in synthetic mode.
  std::printf("[4-6] (synthetic mode: NNF login + MTBT subscribe skipped)\n");

  // [9] Enable strategy at 09:15 -- here we simulate a mid-session clock.
  pipe.set_now(hft::ist_time_of_day_ns(hft::ist_now_ns(), 9, 30, 0));
  std::printf("[9] strategy enabled (simulated 09:30 IST)\n\n");

  std::printf("=== running synthetic session (Ctrl-C / SIGTERM to stop) ===\n");
  // Anchor synthetic quotes around the instrument's previous close so they sit
  // inside the circuit band.
  const hft::Instrument* inst = symbols.get(kToken);
  const hft::Price mid = (inst != nullptr && inst->prev_close > 0) ? inst->prev_close : 1000000;
  uint64_t seq = 1;
  const uint64_t kMaxEvents = 500'000;
  while (g_running.load(std::memory_order_acquire) && seq <= kMaxEvents) {
    const hft::Price base = mid + static_cast<hft::Price>(seq % 100) * 5;
    pipe.process(SnapQuote(kToken, base, base + 10, seq));
    // Simulate fast NSE acks/fills so the order table stays bounded.
    for (const hft::Order& o : pipe.last_sent()) {
      hft::FillReport f{};
      f.client_order_id = o.client_order_id;
      f.token = o.token;
      f.side = o.side;
      f.fill_qty = o.qty;
      f.fill_price = o.price;
      f.remaining_qty = 0;
      pipe.on_fill(f);
    }
    ++seq;
  }

  // Shutdown summary.
  const hft::PipelineStats& s = pipe.stats();
  std::printf("\n=== shutdown summary ===\n");
  std::printf("events=%llu generated=%llu approved=%llu rejected=%llu sent=%llu\n",
              static_cast<unsigned long long>(s.events),
              static_cast<unsigned long long>(s.orders_generated),
              static_cast<unsigned long long>(s.orders_approved),
              static_cast<unsigned long long>(s.orders_rejected),
              static_cast<unsigned long long>(s.orders_sent));
  std::printf("tick-to-trade: p50=%lluns p99=%lluns p999=%lluns (n=%llu)\n",
              static_cast<unsigned long long>(pipe.latency().p50()),
              static_cast<unsigned long long>(pipe.latency().p99()),
              static_cast<unsigned long long>(pipe.latency().p999()),
              static_cast<unsigned long long>(pipe.latency().count()));
  std::printf("(NOTE: untuned host -- colocated BKC targets are p99 < 5us)\n");
  return 0;
}
