// Benchmark: full tick-to-trade pipeline (book -> strategy -> risk -> encode).
#include "pipeline.hpp"

#include <benchmark/benchmark.h>

#include "common/time_utils.hpp"
#include "order_book/book_manager.hpp"
#include "order_manager/order_manager.hpp"
#include "risk/risk_manager.hpp"
#include "session/symbol_master.hpp"
#include "strategy/market_making.hpp"

namespace {

hft::SymbolMaster MakeSymbols() {
  hft::SymbolMaster sm;
  hft::Instrument cm{};
  cm.token = 2885;
  cm.segment = hft::kSegmentCm;
  cm.instrument_type = hft::InstrumentType::kEquity;
  cm.lot_size = 1;
  cm.tick_size = 5;
  cm.prev_close = 1000000;
  cm.circuit_pct_x100 = 2000;
  sm.put(cm);
  return sm;
}

hft::MarketEvent SnapQuote(hft::Price bid, hft::Price ask, uint64_t seq) {
  hft::MarketEvent ev{};
  ev.type = static_cast<uint8_t>(hft::MtbtMsgType::kSnapQuote);
  ev.token = 2885;
  ev.bid_price = bid;
  ev.bid_qty = 100;
  ev.ask_price = ask;
  ev.ask_qty = 100;
  ev.seq_no = seq;
  return ev;
}

void BM_Pipeline(benchmark::State& state) {
  hft::SymbolMaster sm = MakeSymbols();
  hft::BookManager books;
  books.add_token(2885, 5);
  hft::MarketMakingConfig mm_cfg;
  mm_cfg.max_position_lots = 1'000'000;
  hft::MarketMaking strat(&sm, mm_cfg);
  hft::RiskConfig risk_cfg;
  risk_cfg.max_orders_per_sec = 1'000'000'000;
  risk_cfg.max_token_position_lots = 1'000'000;
  risk_cfg.price_sanity_pct = 50;
  hft::RiskManager risk(&sm, risk_cfg);
  hft::OrderManager om;
  hft::Pipeline pipe(&books, &strat, &risk, &om);
  pipe.set_now(hft::ist_time_of_day_ns(hft::ist_now_ns(), 9, 30, 0));

  uint64_t seq = 1;
  for (auto _ : state) {
    const hft::Price base = 1000000 + static_cast<hft::Price>(seq % 100) * 5;
    pipe.process(SnapQuote(base, base + 10, seq));
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
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Pipeline)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
