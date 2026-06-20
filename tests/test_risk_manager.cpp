// Milestone 5 tests: SEBI pre-trade risk checks.
#include "risk/risk_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "common/time_utils.hpp"
#include "session/symbol_master.hpp"

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
  cm.prev_close = 1000000;     // Rs 10,000.00.
  cm.circuit_pct_x100 = 2000;  // 20%.
  sm.put(cm);

  Instrument fo{};
  fo.token = 26000;
  fo.segment = kSegmentFo;
  fo.instrument_type = InstrumentType::kFutIdx;
  fo.lot_size = 25;
  fo.tick_size = 5;
  fo.prev_close = 2400000000;
  fo.circuit_pct_x100 = 1000;
  sm.put(fo);
  return sm;
}

Timestamp At(int h, int m, int s) { return ist_time_of_day_ns(ist_now_ns(), h, m, s); }

Order MakeOrder(Token token, Segment seg, Side side, Price price, Quantity qty) {
  Order o{};
  o.client_order_id = 1;
  o.token = token;
  o.segment = seg;
  o.side = side;
  o.price = price;
  o.qty = qty;
  o.order_type = OrderType::kLimit;
  o.validity = Validity::kDay;
  return o;
}

// 1. Valid order at 09:30 passes all checks.
TEST(RiskManager, ValidApproved) {
  SymbolMaster sm = MakeSymbols();
  RiskManager rm(&sm, {});
  rm.set_now(At(9, 30, 0));
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1000000, 100)),
            RejectReason::kApproved);
}

// 2 & 3. Outside market hours.
TEST(RiskManager, OutsideMarketHours) {
  SymbolMaster sm = MakeSymbols();
  RiskManager rm(&sm, {});
  rm.set_now(At(8, 50, 0));
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1000000, 100)),
            RejectReason::kOutsideMarketHours);
  rm.set_now(At(15, 35, 0));
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1000000, 100)),
            RejectReason::kOutsideMarketHours);
}

// 4. Rate limit: the 1001st order in a fixed instant is rejected.
TEST(RiskManager, RateLimit) {
  SymbolMaster sm = MakeSymbols();
  RiskConfig cfg;
  cfg.max_orders_per_sec = 1000;
  RiskManager rm(&sm, cfg);
  rm.set_now(At(9, 30, 0));
  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1000000, 1)),
              RejectReason::kApproved);
  }
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1000000, 1)),
            RejectReason::kRateLimitExceeded);
}

// 5. Price band breach (25% above prev close on a 20% band stock).
TEST(RiskManager, PriceBandReject) {
  SymbolMaster sm = MakeSymbols();
  RiskManager rm(&sm, {});
  rm.set_now(At(9, 30, 0));
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1250000, 100)),
            RejectReason::kPriceBandBreach);
}

// 6. Price 15% above prev close (within 20% band) is approved.
TEST(RiskManager, PriceBandApprove) {
  SymbolMaster sm = MakeSymbols();
  RiskManager rm(&sm, {});
  rm.set_now(At(9, 30, 0));
  // No reference set -> sanity skipped; band (15% < 20%) passes.
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1150000, 100)),
            RejectReason::kApproved);
}

// 7. Price sanity: 5% above best ask is rejected.
TEST(RiskManager, PriceSanityReject) {
  SymbolMaster sm = MakeSymbols();
  RiskManager rm(&sm, {});
  rm.set_now(At(9, 30, 0));
  rm.update_reference(2885, 999500, 1000000);  // best ask 10,000.00.
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1050000, 100)),
            RejectReason::kPriceSanity);
}

// 8. Position limit (FO).
TEST(RiskManager, PositionLimit) {
  SymbolMaster sm = MakeSymbols();
  RiskConfig cfg;
  cfg.max_token_position_lots = 4;  // 4 lots * 25 = 100 contracts.
  RiskManager rm(&sm, cfg);
  rm.set_now(At(9, 30, 0));
  // 125 contracts = 5 lots > 4 -> reject.
  EXPECT_EQ(rm.check(MakeOrder(26000, kSegmentFo, kBuy, 2400000000, 125)),
            RejectReason::kPositionLimit);
}

// 9. Lot-size mismatch (FO).
TEST(RiskManager, LotSizeMismatch) {
  SymbolMaster sm = MakeSymbols();
  RiskManager rm(&sm, {});
  rm.set_now(At(9, 30, 0));
  EXPECT_EQ(rm.check(MakeOrder(26000, kSegmentFo, kBuy, 2400000000, 30)),
            RejectReason::kLotSizeMismatch);
}

// 10. Correct lot multiple (FO) approved.
TEST(RiskManager, LotSizeOk) {
  SymbolMaster sm = MakeSymbols();
  RiskManager rm(&sm, {});
  rm.set_now(At(9, 30, 0));
  EXPECT_EQ(rm.check(MakeOrder(26000, kSegmentFo, kBuy, 2400000000, 25)),
            RejectReason::kApproved);
}

// 11. CM order size over the cap.
TEST(RiskManager, OrderTooLarge) {
  SymbolMaster sm = MakeSymbols();
  RiskConfig cfg;
  cfg.max_order_qty = 10000;
  RiskManager rm(&sm, cfg);
  rm.set_now(At(9, 30, 0));
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1000000, 10001)),
            RejectReason::kOrderTooLarge);
}

// 12. Daily-loss kill switch auto-trips.
TEST(RiskManager, DailyLossKill) {
  SymbolMaster sm = MakeSymbols();
  RiskConfig cfg;
  cfg.max_daily_loss_inr = 1000;
  RiskManager rm(&sm, cfg);
  rm.set_now(At(9, 30, 0));
  // Buy 100 @ Rs 10,000 then mark collapses to Rs 9,000 -> Rs 100,000 loss.
  FillReport f{};
  f.token = 2885;
  f.side = kBuy;
  f.fill_qty = 100;
  f.fill_price = 1000000;
  f.remaining_qty = 0;
  rm.on_fill(f);
  // Re-mark lower via a tiny sell fill at a much lower price.
  FillReport mark{};
  mark.token = 2885;
  mark.side = kSell;
  mark.fill_qty = 0;  // qty 0 just re-marks.
  mark.fill_price = 900000;
  rm.on_fill(mark);
  EXPECT_TRUE(rm.kill_switch_active());
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1000000, 1)),
            RejectReason::kKillSwitch);
}

// 13 & 14. Manual kill switch set / clear.
TEST(RiskManager, ManualKillSwitch) {
  SymbolMaster sm = MakeSymbols();
  RiskManager rm(&sm, {});
  rm.set_now(At(9, 30, 0));
  rm.set_kill_switch(true);
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1000000, 1)),
            RejectReason::kKillSwitch);
  rm.set_kill_switch(false);
  EXPECT_EQ(rm.check(MakeOrder(2885, kSegmentCm, kBuy, 1000000, 1)),
            RejectReason::kApproved);
}

// 15. Position updates on fills.
TEST(RiskManager, PositionUpdate) {
  SymbolMaster sm = MakeSymbols();
  RiskManager rm(&sm, {});
  FillReport f{};
  f.token = 26000;
  f.side = kBuy;
  f.fill_qty = 50;
  f.fill_price = 2400000000;
  rm.on_fill(f);
  EXPECT_EQ(rm.position(26000), 50);
}

// 16. check() latency budget.
TEST(RiskManager, Latency) {
  SymbolMaster sm = MakeSymbols();
  RiskConfig cfg;
  cfg.max_orders_per_sec = 100'000'000;  // Do not let the bucket gate timing.
  RiskManager rm(&sm, cfg);
  rm.set_now(At(9, 30, 0));
  Order o = MakeOrder(2885, kSegmentCm, kBuy, 1000000, 1);
  rm.check(o);  // Warm.
  const TscClock& clk = TscClock::Instance();
  uint64_t best = UINT64_MAX;
  for (int i = 0; i < 1000; ++i) {
    uint64_t t0 = rdtsc();
    rm.check(o);
    best = std::min(best, clk.to_ns(rdtsc() - t0));
  }
  EXPECT_LT(best, 1000u);  // Generous bound for untuned CI hardware.
}

// 17. Concurrent submit + kill-switch toggle: no crash / data race.
TEST(RiskManager, ConcurrentKillToggle) {
  SymbolMaster sm = MakeSymbols();
  RiskConfig cfg;
  cfg.max_orders_per_sec = 1'000'000'000;  // Do not let the bucket gate this test.
  RiskManager rm(&sm, cfg);
  rm.set_now(At(9, 30, 0));
  std::atomic<bool> stop{false};
  std::thread toggler([&] {
    while (!stop.load()) {
      rm.set_kill_switch(true);
      rm.set_kill_switch(false);
    }
  });
  Order o = MakeOrder(2885, kSegmentCm, kBuy, 1000000, 1);
  for (int i = 0; i < 200000; ++i) {
    RejectReason r = rm.check(o);
    EXPECT_TRUE(r == RejectReason::kApproved || r == RejectReason::kKillSwitch);
  }
  stop.store(true);
  toggler.join();
}

}  // namespace
}  // namespace hft
