// Milestone 8 test: end-to-end tick-to-trade latency over a synthetic replay.
//
// On untuned CI hardware we cannot assert the colocated p99 < 5us target, so
// this test validates that the full pipeline runs cleanly, generates and sends
// orders, records latency samples and reports a sane percentile distribution.
// The benchmarks/bench_pipeline binary reports the actual numbers.
#include "pipeline.hpp"

#include <gtest/gtest.h>

#include <cstdio>

#include "common/time_utils.hpp"
#include "order_book/book_manager.hpp"
#include "order_manager/order_manager.hpp"
#include "risk/risk_manager.hpp"
#include "session/symbol_master.hpp"
#include "strategy/market_making.hpp"

namespace hft {
namespace {

SymbolMaster MakeSymbols() {
  SymbolMaster sm;
  Instrument cm{};
  cm.token = 2885;
  cm.segment = kSegmentCm;
  cm.instrument_type = InstrumentType::kEquity;
  cm.lot_size = 1;
  cm.tick_size = 5;
  cm.prev_close = 1000000;
  cm.circuit_pct_x100 = 2000;  // 20% band -> our small moves stay in band.
  sm.put(cm);
  return sm;
}

// A Snap Quote re-seeds top of book each iteration (so the book never grows)
// with a slightly moved bid/ask, prompting the strategy to re-quote.
MarketEvent SnapQuote(Token token, Price bid, Price ask, uint64_t seq) {
  MarketEvent ev{};
  ev.type = static_cast<uint8_t>(MtbtMsgType::kSnapQuote);
  ev.token = token;
  ev.bid_price = bid;
  ev.bid_qty = 100;
  ev.ask_price = ask;
  ev.ask_qty = 100;
  ev.seq_no = seq;
  return ev;
}

TEST(Latency, EndToEndReplay) {
  SymbolMaster sm = MakeSymbols();
  BookManager books;
  books.add_token(2885, /*tick=*/5);

  MarketMakingConfig mm_cfg;
  mm_cfg.max_position_lots = 1'000'000;  // Position is netted by fills below.
  MarketMaking strat(&sm, mm_cfg);

  RiskConfig risk_cfg;
  risk_cfg.max_orders_per_sec = 1'000'000'000;  // Do not gate the benchmark.
  risk_cfg.max_token_position_lots = 1'000'000;
  risk_cfg.price_sanity_pct = 50;  // Allow our 1-tick-inside quotes.
  RiskManager risk(&sm, risk_cfg);

  OrderManager om;
  Pipeline pipe(&books, &strat, &risk, &om);
  pipe.set_now(ist_time_of_day_ns(ist_now_ns(), 9, 30, 0));

  constexpr int kEvents = 200'000;
  for (int i = 0; i < kEvents; ++i) {
    const Price base = 1000000 + (i % 100) * 5;  // Walk within the band.
    pipe.process(SnapQuote(2885, base, base + 10, static_cast<uint64_t>(i + 1)));
    // Terminalize sent orders (simulate fast NSE acks/fills) so the order
    // table and position stay bounded and net to ~flat.
    for (const Order& o : pipe.last_sent()) {
      FillReport f{};
      f.client_order_id = o.client_order_id;
      f.token = o.token;
      f.side = o.side;
      f.fill_qty = o.qty;
      f.fill_price = o.price;
      f.remaining_qty = 0;
      pipe.on_fill(f);
    }
  }

  const PipelineStats& s = pipe.stats();
  EXPECT_EQ(s.events, static_cast<uint64_t>(kEvents));
  EXPECT_GT(s.orders_sent, 0u);
  EXPECT_GT(pipe.latency().count(), 0u);

  // Sanity: percentiles are ordered and finite.
  const uint64_t p50 = pipe.latency().p50();
  const uint64_t p99 = pipe.latency().p99();
  const uint64_t p999 = pipe.latency().p999();
  EXPECT_LE(p50, p99);
  EXPECT_LE(p99, p999);
  std::printf("[tick-to-trade] sent=%llu p50=%lluns p99=%lluns p999=%lluns\n",
              static_cast<unsigned long long>(s.orders_sent),
              static_cast<unsigned long long>(p50), static_cast<unsigned long long>(p99),
              static_cast<unsigned long long>(p999));
  // Very generous ceiling: catch pathological regressions only.
  EXPECT_LT(p50, 100'000u);
}

}  // namespace
}  // namespace hft
