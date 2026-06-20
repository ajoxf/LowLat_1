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

int main(int argc, char** argv) {
  const char* capture = argc >= 2 ? argv[1] : nullptr;

  // Instrument: RELIANCE-like, wide band so synthetic walk stays tradable.
  hft::SymbolMaster symbols;
  hft::Instrument cm{};
  cm.token = kToken;
  cm.segment = hft::kSegmentCm;
  cm.instrument_type = hft::InstrumentType::kEquity;
  cm.lot_size = 1;
  cm.tick_size = kTick;
  cm.prev_close = kMid;
  cm.circuit_pct_x100 = 2000;  // 20%.
  symbols.put(cm);

  hft::BookManager books;
  books.add_token(kToken, kTick);

  hft::MarketMakingConfig mm_cfg;
  mm_cfg.max_position_lots = 50;
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
  std::vector<hft::MarketEvent> events = LoadEvents(capture);

  // Simple mark-to-market PnL accounting (paisa).
  int64_t cash_paisa = 0;
  int64_t position = 0;
  hft::Price mark = kMid;
  uint64_t maker_fills = 0;

  for (const hft::MarketEvent& ev : events) {
    if (static_cast<hft::MtbtMsgType>(ev.type) == hft::MtbtMsgType::kSnapQuote) {
      mark = (ev.bid_price + ev.ask_price) / 2;
    }
    // 1. Fill our existing resting quotes against this event.
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
    // 2. Process the event -> update book, generate new quotes.
    pipe.process(ev);
    // 3. Register newly-sent quotes with the fill simulator.
    for (const hft::Order& o : pipe.last_sent()) {
      fillsim.register_order(o);
    }
  }

  const int64_t pnl_paisa = cash_paisa + position * mark;
  const hft::PipelineStats& s = pipe.stats();

  std::printf("\n=== backtest report ===\n");
  std::printf("events        : %llu\n", static_cast<unsigned long long>(s.events));
  std::printf("orders sent   : %llu\n", static_cast<unsigned long long>(s.orders_sent));
  std::printf("orders reject : %llu (risk)\n",
              static_cast<unsigned long long>(s.orders_rejected));
  std::printf("maker fills   : %llu\n", static_cast<unsigned long long>(maker_fills));
  std::printf("final position: %lld units\n", static_cast<long long>(position));
  std::printf("gross PnL     : Rs %.2f (mark-to-market @ %lld paisa)\n",
              static_cast<double>(pnl_paisa) / 100.0, static_cast<long long>(mark));
  std::printf("tick-to-trade : p50=%lluns p99=%lluns p999=%lluns (n=%llu)\n",
              static_cast<unsigned long long>(pipe.latency().p50()),
              static_cast<unsigned long long>(pipe.latency().p99()),
              static_cast<unsigned long long>(pipe.latency().p999()),
              static_cast<unsigned long long>(pipe.latency().count()));
  std::printf("\nNOTE: optimistic front-of-queue fill model + synthetic data.\n");
  std::printf("Use recorded NSE MTBT and a queue-aware model before trusting PnL.\n");
  return 0;
}
