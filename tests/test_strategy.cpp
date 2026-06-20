// Milestone 4 tests: market-making strategy.
#include "strategy/market_making.hpp"

#include <gtest/gtest.h>

#include "common/time_utils.hpp"
#include "order_book/order_book.hpp"
#include "session/symbol_master.hpp"

namespace hft {
namespace {

// CM equity token 2885 (tick 5, lot 1) and FO Nifty future 26000 (tick 5,
// lot 25). prev_close / circuit give a wide band so normal quotes pass.
SymbolMaster MakeSymbols() {
  SymbolMaster sm;
  Instrument cm{};
  cm.token = 2885;
  cm.segment = kSegmentCm;
  cm.instrument_type = InstrumentType::kEquity;
  cm.lot_size = 1;
  cm.tick_size = 5;
  cm.prev_close = 1000000;       // Rs 10,000.00 in paisa.
  cm.circuit_pct_x100 = 2000;    // 20% band.
  sm.put(cm);

  Instrument fo{};
  fo.token = 26000;
  fo.segment = kSegmentFo;
  fo.instrument_type = InstrumentType::kFutIdx;
  fo.lot_size = 25;
  fo.tick_size = 5;
  fo.prev_close = 2400000000;
  fo.circuit_pct_x100 = 1000;    // 10% band.
  sm.put(fo);
  return sm;
}

MarketEvent Add(int64_t no, Side side, Price price, Quantity qty, Token token) {
  MarketEvent ev{};
  ev.type = static_cast<uint8_t>(MtbtMsgType::kOrderAdd);
  ev.order_no = no;
  ev.side = side;
  ev.price = price;
  ev.qty = qty;
  ev.token = token;
  return ev;
}

Timestamp At(int h, int m, int s) { return ist_time_of_day_ns(ist_now_ns(), h, m, s); }

// Seed a two-sided book around `mid` for `token`.
void SeedBook(OrderBook& book, Token token, Price bid, Price ask) {
  book.apply_event(Add(1, kBuy, bid, 100, token));
  book.apply_event(Add(2, kSell, ask, 100, token));
}

// 1. Flat book: no quotes.
TEST(MarketMaking, FlatBookNoQuote) {
  SymbolMaster sm = MakeSymbols();
  MarketMaking mm(&sm, {});
  mm.set_now(At(9, 30, 0));
  OrderBook book(2885, 5);
  mm.on_book_update(2885, book);
  EXPECT_TRUE(mm.get_pending_orders().empty());
}

// 2. Normal CM market: bid one tick below, ask one tick above.
TEST(MarketMaking, QuotesInsideTouch) {
  SymbolMaster sm = MakeSymbols();
  MarketMaking mm(&sm, {});
  mm.set_now(At(9, 30, 0));
  OrderBook book(2885, 5);
  SeedBook(book, 2885, 1000000, 1000010);  // bid 10000.00, ask 10000.10.
  mm.on_book_update(2885, book);
  auto orders = mm.get_pending_orders();
  ASSERT_EQ(orders.size(), 2u);
  // One buy at 1000000-5, one sell at 1000010+5.
  bool saw_bid = false;
  bool saw_ask = false;
  for (const Order& o : orders) {
    if (o.side == kBuy) {
      EXPECT_EQ(o.price, 1000000 - 5);
      saw_bid = true;
    } else {
      EXPECT_EQ(o.price, 1000010 + 5);
      saw_ask = true;
    }
  }
  EXPECT_TRUE(saw_bid && saw_ask);
}

// 3. FO quantities are lot multiples.
TEST(MarketMaking, FoLotMultiples) {
  SymbolMaster sm = MakeSymbols();
  MarketMakingConfig cfg;
  cfg.segment = kSegmentFo;
  cfg.quote_lots = 2;
  MarketMaking mm(&sm, cfg);
  mm.set_now(At(9, 30, 0));
  OrderBook book(26000, 5);
  SeedBook(book, 26000, 2400000000, 2400000010);  // 2-tick spread.
  mm.on_book_update(26000, book);
  auto orders = mm.get_pending_orders();
  ASSERT_FALSE(orders.empty());
  for (const Order& o : orders) {
    EXPECT_EQ(o.qty % 25, 0);
    EXPECT_EQ(o.qty, 50);  // 2 lots * 25.
  }
}

// 4. Spread too wide: stay silent.
TEST(MarketMaking, SpreadTooWide) {
  SymbolMaster sm = MakeSymbols();
  MarketMakingConfig cfg;
  cfg.max_spread_ticks = 3;
  MarketMaking mm(&sm, cfg);
  mm.set_now(At(9, 30, 0));
  OrderBook book(2885, 5);
  SeedBook(book, 2885, 1000000, 1000100);  // spread 100 = 20 ticks.
  mm.on_book_update(2885, book);
  EXPECT_TRUE(mm.get_pending_orders().empty());
}

// 5. Max long: only the ask is sent.
TEST(MarketMaking, MaxLongOnlyAsk) {
  SymbolMaster sm = MakeSymbols();
  MarketMakingConfig cfg;
  cfg.max_position_lots = 10;
  MarketMaking mm(&sm, cfg);
  mm.set_now(At(9, 30, 0));
  // Drive position to +10 (CM lot 1).
  FillReport f{};
  f.token = 2885;
  f.side = kBuy;
  f.fill_qty = 10;
  f.remaining_qty = 0;
  mm.on_fill(f);
  OrderBook book(2885, 5);
  SeedBook(book, 2885, 1000000, 1000010);
  mm.on_book_update(2885, book);
  auto orders = mm.get_pending_orders();
  ASSERT_EQ(orders.size(), 1u);
  EXPECT_EQ(orders[0].side, kSell);
}

// 6. Max short: only the bid is sent.
TEST(MarketMaking, MaxShortOnlyBid) {
  SymbolMaster sm = MakeSymbols();
  MarketMakingConfig cfg;
  cfg.max_position_lots = 10;
  MarketMaking mm(&sm, cfg);
  mm.set_now(At(9, 30, 0));
  FillReport f{};
  f.token = 2885;
  f.side = kSell;
  f.fill_qty = 10;
  f.remaining_qty = 0;
  mm.on_fill(f);
  OrderBook book(2885, 5);
  SeedBook(book, 2885, 1000000, 1000010);
  mm.on_book_update(2885, book);
  auto orders = mm.get_pending_orders();
  ASSERT_EQ(orders.size(), 1u);
  EXPECT_EQ(orders[0].side, kBuy);
}

// 7. Stale quote past TTL is cancelled and refreshed.
TEST(MarketMaking, StaleQuoteCancelled) {
  SymbolMaster sm = MakeSymbols();
  MarketMakingConfig cfg;
  cfg.quote_ttl_ns = 1000;
  MarketMaking mm(&sm, cfg);
  OrderBook book(2885, 5);
  SeedBook(book, 2885, 1000000, 1000010);
  mm.set_now(At(9, 30, 0));
  mm.on_book_update(2885, book);
  ASSERT_EQ(mm.get_pending_orders().size(), 2u);
  EXPECT_TRUE(mm.get_pending_cancels().empty());
  // Advance past TTL; same book -> stale quotes cancelled and re-placed.
  mm.set_now(At(9, 30, 0) + 5000);
  mm.on_book_update(2885, book);
  EXPECT_EQ(mm.get_pending_cancels().size(), 2u);
  EXPECT_EQ(mm.get_pending_orders().size(), 2u);
}

// 8. Fill updates position by signed quantity.
TEST(MarketMaking, FillUpdatesPosition) {
  SymbolMaster sm = MakeSymbols();
  MarketMaking mm(&sm, {});
  FillReport f{};
  f.token = 26000;
  f.side = kBuy;
  f.fill_qty = 25;
  f.remaining_qty = 0;
  mm.on_fill(f);
  EXPECT_EQ(mm.position(26000), 25);
  f.side = kSell;
  f.fill_qty = 10;
  mm.on_fill(f);
  EXPECT_EQ(mm.position(26000), 15);
}

// 9. Pre-open (09:05): no live orders.
TEST(MarketMaking, PreOpenNoOrders) {
  SymbolMaster sm = MakeSymbols();
  MarketMaking mm(&sm, {});
  mm.set_now(At(9, 5, 0));
  OrderBook book(2885, 5);
  SeedBook(book, 2885, 1000000, 1000010);
  mm.on_book_update(2885, book);
  EXPECT_TRUE(mm.get_pending_orders().empty());
}

// 10. After 15:25: cancel all, place nothing.
TEST(MarketMaking, PreCloseCancelSweep) {
  SymbolMaster sm = MakeSymbols();
  MarketMaking mm(&sm, {});
  OrderBook book(2885, 5);
  SeedBook(book, 2885, 1000000, 1000010);
  mm.set_now(At(9, 30, 0));
  mm.on_book_update(2885, book);  // Establish quotes.
  ASSERT_EQ(mm.get_pending_orders().size(), 2u);
  mm.set_now(At(15, 26, 0));
  mm.on_book_update(2885, book);
  EXPECT_TRUE(mm.get_pending_orders().empty());
  EXPECT_EQ(mm.get_pending_cancels().size(), 2u);
}

// 11. Price band breach: that side is not quoted.
TEST(MarketMaking, PriceBandBreach) {
  SymbolMaster sm = MakeSymbols();
  // Tight band: 1% around prev_close 1,000,000 -> [990000, 1010000].
  Instrument cm = *sm.get(2885);
  cm.circuit_pct_x100 = 100;  // 1%.
  sm.put(cm);
  MarketMaking mm(&sm, {});
  mm.set_now(At(9, 30, 0));
  OrderBook book(2885, 5);
  // Ask side desired = 1011000 (above band high 1010000) -> ask suppressed.
  SeedBook(book, 2885, 1000000, 1010995);
  mm.on_book_update(2885, book);
  for (const Order& o : mm.get_pending_orders()) {
    EXPECT_EQ(o.side, kBuy);  // Only the in-band bid is quoted.
  }
}

// 12. Rapid updates: no crash, consistent output.
TEST(MarketMaking, RapidUpdates) {
  SymbolMaster sm = MakeSymbols();
  MarketMaking mm(&sm, {});
  mm.set_now(At(9, 30, 0));
  OrderBook book(2885, 5);
  for (int i = 0; i < 1000; ++i) {
    OrderBook b(2885, 5);
    SeedBook(b, 2885, 1000000 + (i % 5) * 5, 1000010 + (i % 5) * 5);
    mm.on_book_update(2885, b);
    EXPECT_LE(mm.get_pending_orders().size(), 2u);
  }
}

// Helper: pull the (bid_price, ask_price) from the strategy's pending orders.
void QuotedPrices(MarketMaking& mm, Price* bid, Price* ask) {
  *bid = 0;
  *ask = 0;
  for (const Order& o : mm.get_pending_orders()) {
    if (o.side == kBuy) *bid = o.price;
    else *ask = o.price;
  }
}

// Inventory skew: a long position shifts BOTH quotes down (to offload).
TEST(MarketMaking, InventorySkewLeansAgainstPosition) {
  SymbolMaster sm = MakeSymbols();
  MarketMakingConfig cfg;
  cfg.max_position_lots = 10;
  cfg.max_inv_skew_ticks = 2;  // CM lot 1 -> max_pos_units = 10.
  MarketMaking mm(&sm, cfg);
  mm.set_now(At(9, 30, 0));

  OrderBook book(2885, 5);
  SeedBook(book, 2885, 1000000, 1000010);

  // Flat: naive quotes (bid 999995, ask 1000015).
  mm.on_book_update(2885, book);
  Price flat_bid, flat_ask;
  QuotedPrices(mm, &flat_bid, &flat_ask);
  EXPECT_EQ(flat_bid, 999995);
  EXPECT_EQ(flat_ask, 1000015);

  // Go +5 long -> inv skew = 5*2/10 = 1 tick down on both sides.
  FillReport f{};
  f.token = 2885;
  f.side = kBuy;
  f.fill_qty = 5;
  f.remaining_qty = 0;
  mm.on_fill(f);
  mm.on_book_update(2885, book);
  Price long_bid, long_ask;
  QuotedPrices(mm, &long_bid, &long_ask);
  EXPECT_EQ(long_bid, 999990);  // shifted down one tick
  EXPECT_EQ(long_ask, 1000010);
}

// Order-flow skew: a bid-heavy book shifts BOTH quotes up (ride the pressure).
TEST(MarketMaking, OrderFlowSkewLeansWithPressure) {
  SymbolMaster sm = MakeSymbols();
  MarketMakingConfig cfg;
  cfg.max_flow_skew_ticks = 2;
  MarketMaking mm(&sm, cfg);
  mm.set_now(At(9, 30, 0));

  // Balanced book: equal size -> no flow skew -> naive quotes.
  OrderBook balanced(2885, 5);
  balanced.apply_event(Add(1, kBuy, 1000000, 100, 2885));
  balanced.apply_event(Add(2, kSell, 1000010, 100, 2885));
  mm.on_book_update(2885, balanced);
  Price bal_bid, bal_ask;
  QuotedPrices(mm, &bal_bid, &bal_ask);
  EXPECT_EQ(bal_bid, 999995);
  EXPECT_EQ(bal_ask, 1000015);

  // Bid-heavy book: 100 vs 10 -> flow = (90)*2/110 = 1 tick up.
  OrderBook heavy(2885, 5);
  heavy.apply_event(Add(3, kBuy, 1000000, 100, 2885));
  heavy.apply_event(Add(4, kSell, 1000010, 10, 2885));
  MarketMaking mm2(&sm, cfg);
  mm2.set_now(At(9, 30, 0));
  mm2.on_book_update(2885, heavy);
  Price hv_bid, hv_ask;
  QuotedPrices(mm2, &hv_bid, &hv_ask);
  EXPECT_GT(hv_bid, bal_bid);  // shifted up
  EXPECT_GT(hv_ask, bal_ask);
}

// 14. on_book_update latency budget.
TEST(MarketMaking, Latency) {
  SymbolMaster sm = MakeSymbols();
  MarketMaking mm(&sm, {});
  mm.set_now(At(9, 30, 0));
  OrderBook book(2885, 5);
  SeedBook(book, 2885, 1000000, 1000010);
  mm.on_book_update(2885, book);  // Warm.
  const TscClock& clk = TscClock::Instance();
  uint64_t best = UINT64_MAX;
  for (int i = 0; i < 500; ++i) {
    uint64_t t0 = rdtsc();
    mm.on_book_update(2885, book);
    best = std::min(best, clk.to_ns(rdtsc() - t0));
  }
  EXPECT_LT(best, 3000u);  // Generous bound for untuned CI hardware.
}

}  // namespace
}  // namespace hft
