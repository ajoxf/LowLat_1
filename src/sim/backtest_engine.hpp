// Reusable backtest engine: runs the full tick-to-trade pipeline (book ->
// strategy -> risk -> OMS) with the queue-aware fill simulator over a stream of
// market events, and returns simulated PnL + latency. Shared by the `backtest`
// comparison tool and the `sweep` parameter-search tool so both measure the
// identical code path.
#ifndef HFT_SIM_BACKTEST_ENGINE_HPP_
#define HFT_SIM_BACKTEST_ENGINE_HPP_

#include <cstdint>
#include <vector>

#include "common/time_utils.hpp"
#include "common/types.hpp"
#include "monitor/metrics.hpp"
#include "order_book/book_manager.hpp"
#include "order_manager/order_manager.hpp"
#include "pipeline.hpp"
#include "risk/risk_manager.hpp"
#include "session/symbol_master.hpp"
#include "sim/fill_simulator.hpp"
#include "strategy/market_making.hpp"

namespace hft {

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

// Run one backtest over `events` for instrument (token, tick, prev_close=mid)
// with strategy config `mm_cfg`. If `metrics_path` is non-null, a JSONL metrics
// stream is written for the dashboard.
inline BtResult run_backtest(const std::vector<MarketEvent>& events,
                             const MarketMakingConfig& mm_cfg, Token token, Price mid,
                             Price tick, const char* metrics_path) {
  SymbolMaster symbols;
  Instrument cm{};
  cm.token = token;
  cm.segment = kSegmentCm;
  cm.instrument_type = InstrumentType::kEquity;
  cm.lot_size = 1;
  cm.tick_size = tick;
  cm.prev_close = mid;
  cm.circuit_pct_x100 = 2000;  // 20% band: keep synthetic walk tradable.
  symbols.put(cm);

  BookManager books;
  books.add_token(token, tick);
  MarketMaking strat(&symbols, mm_cfg);

  RiskConfig risk_cfg;
  risk_cfg.max_orders_per_sec = 1'000'000'000;  // Do not throttle the backtest.
  risk_cfg.max_token_position_lots = 1'000'000;
  risk_cfg.price_sanity_pct = 50;
  RiskManager risk(&symbols, risk_cfg);

  OrderManager om;
  Pipeline pipe(&books, &strat, &risk, &om);
  pipe.set_now(ist_time_of_day_ns(ist_now_ns(), 10, 0, 0));

  FillSimulator fillsim;
  MetricsWriter metrics;
  const bool want_metrics = metrics_path != nullptr && metrics.open(metrics_path);
  const uint64_t snapshot_every = events.empty() ? 1 : (events.size() / 200) + 1;

  int64_t cash_paisa = 0;
  int64_t position = 0;
  Price mark = mid;
  uint64_t maker_fills = 0;
  uint64_t event_index = 0;

  for (const MarketEvent& ev : events) {
    if (static_cast<MtbtMsgType>(ev.type) == MtbtMsgType::kSnapQuote) {
      mark = (ev.bid_price + ev.ask_price) / 2;
    } else if (static_cast<MtbtMsgType>(ev.type) == MtbtMsgType::kTrade) {
      mark = ev.price;  // MBO streams carry the last trade price.
    }
    fillsim.on_event(ev, [&](const FillReport& f) {
      pipe.on_fill(f);
      if (f.side == kBuy) {
        cash_paisa -= static_cast<int64_t>(f.fill_qty) * f.fill_price;
        position += f.fill_qty;
      } else {
        cash_paisa += static_cast<int64_t>(f.fill_qty) * f.fill_price;
        position -= f.fill_qty;
      }
      ++maker_fills;
    });
    pipe.process(ev);
    OrderBook* book = books.get_book(token);
    for (const Order& o : pipe.last_sent()) {
      const Quantity queue_ahead =
          book != nullptr ? book->displayed_qty_at(o.side, o.price) : 0;
      fillsim.register_order(o, queue_ahead);
    }
    ++event_index;
    if (want_metrics && event_index % snapshot_every == 0) {
      MetricSnapshot snap;
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

}  // namespace hft

#endif  // HFT_SIM_BACKTEST_ENGINE_HPP_
