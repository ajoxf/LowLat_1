// Tests for the iRage ConnectLib strategy adapter, driven through the mock
// platform (irage/mock/ConnectHeader.h). Compiled as C++17 to match the iRage
// toolchain constraint.
#include "mystrat.h"

#include <gtest/gtest.h>

namespace my_strat_ns {
namespace {

using alpha::ConnectImpl;
using alpha::EXEC_REPORT;
using alpha::MTICK;
using alpha::SIDE_BUY;
using alpha::SIDE_SELL;

constexpr alpha::SymbolIdType kSym = 46483;

// 10:00 IST as UTC epoch seconds-of-day (any day works; the strategy only
// looks at seconds-of-day after the +5:30 shift).
constexpr int kUtc1000Ist = (10 * 3600 + 0 * 60) - 19800;  // 10:00 IST in UTC
constexpr int kUtc1526Ist = (15 * 3600 + 26 * 60) - 19800;  // 15:26 IST in UTC

MTICK MakeTick(int bid, int ask, int bid_sz = 100, int ask_sz = 100) {
  MTICK t;
  t.id = kSym;
  t.bid[0] = bid;
  t.ask[0] = ask;
  t.bid_size[0] = bid_sz;
  t.ask_size[0] = ask_sz;
  return t;
}

struct Harness {
  ConnectImpl impl;
  strat_1 strat;

  Harness() {
    strat.impl = &impl;
    std::map<std::string, std::string> info;
    info["tick_size"] = "5";
    info["lot_size"] = "15";
    info["strike"] = "0";
    info["maturity"] = "0";
    impl.test_setSymInfo(kSym, info);
    impl.test_setInventory(kSym, 0);
    impl.setMaxInventory(kSym, 10);  // 10 lots => 150 units.
    impl.test_setSeconds(kUtc1000Ist);
    std::map<int, std::string> cfg;
    cfg[16] = "1";
    cfg[751] = std::to_string(kSym);
    strat.initialize(cfg);
  }
};

// initialize() reads syminfo and calls impl->init().
TEST(IrageStrat, InitializeReadsSymbols) {
  Harness h;
  EXPECT_TRUE(h.impl.test_inited());
  EXPECT_EQ(h.strat.sym_count, 1);
  EXPECT_EQ(h.strat.syms[0].tick_size, 5);
  EXPECT_EQ(h.strat.syms[0].lot_size, 15);
}

// STOP mode: no orders.
TEST(IrageStrat, NoOrdersWhenStopped) {
  Harness h;
  h.impl.test_setRunning(false);
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  EXPECT_TRUE(h.impl.test_sent().empty());
}

// Flat position, balanced book: two NEW orders one tick inside the touch.
TEST(IrageStrat, QuotesBothSidesInsideTouch) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  auto& sent = h.impl.test_sent();
  ASSERT_EQ(sent.size(), 2u);
  int bid_px = 0;
  int ask_px = 0;
  for (size_t i = 0; i < sent.size(); ++i) {
    EXPECT_EQ(sent[i].type, 'N');
    EXPECT_EQ(sent[i].quote.qty, 15);  // 1 lot.
    if (sent[i].quote.side == SIDE_BUY) {
      bid_px = sent[i].quote.price;
    } else {
      ask_px = sent[i].quote.price;
    }
  }
  EXPECT_EQ(bid_px, 7200 - 5);
  EXPECT_EQ(ask_px, 7210 + 5);
}

// Long inventory shifts both quotes down (inventory skew).
TEST(IrageStrat, InventorySkewShiftsQuotesDown) {
  Harness h;
  h.impl.test_setRunning(true);
  // +75 units of max 150 with inv_skew=2 -> 1 tick down.
  h.impl.test_setInventory(kSym, 75);
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  auto& sent = h.impl.test_sent();
  ASSERT_EQ(sent.size(), 2u);
  for (size_t i = 0; i < sent.size(); ++i) {
    if (sent[i].quote.side == SIDE_BUY) {
      EXPECT_EQ(sent[i].quote.price, 7200 - 5 - 5);
    } else {
      EXPECT_EQ(sent[i].quote.price, 7210 + 5 - 5);
    }
  }
}

// Null/fake tick (packet drop): sanityCheck fails; standing quotes cancelled.
TEST(IrageStrat, NullTickCancelsQuotes) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();  // Establish quotes.
  h.impl.test_ackAll();
  h.impl.test_sent().clear();

  MTICK null_tick = MakeTick(0, 0, 0, 0);
  EXPECT_FALSE(h.strat.sanityCheck(null_tick, 0));
  h.impl.test_setMtick(null_tick);
  h.strat.onMarketData();
  auto& sent = h.impl.test_sent();
  ASSERT_EQ(sent.size(), 2u);  // Two cancels.
  EXPECT_EQ(sent[0].type, 'X');
  EXPECT_EQ(sent[1].type, 'X');
}

// Spread wider than max_spread_ticks: stay out.
TEST(IrageStrat, WideSpreadNoQuote) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setMtick(MakeTick(7200, 7200 + 4 * 5 + 5));  // 5 ticks wide > 3.
  h.strat.onMarketData();
  EXPECT_TRUE(h.impl.test_sent().empty());
}

// While a request is unacked, a second call does not duplicate (E_NO_ACK).
TEST(IrageStrat, NoDuplicateWhileUnacked) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  ASSERT_EQ(h.impl.test_sent().size(), 2u);
  h.strat.onMarketData();  // Same tick, still unacked.
  EXPECT_EQ(h.impl.test_sent().size(), 2u);  // Nothing new.
}

// After acks, a price move produces RPL orders; an unchanged price does not.
TEST(IrageStrat, ReplacesOnMoveHoldsOnSame) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  h.impl.test_ackAll();
  h.impl.test_sent().clear();

  h.strat.onMarketData();  // Same prices: E_SAME_ORD path, no messages.
  EXPECT_TRUE(h.impl.test_sent().empty());

  h.impl.test_setMtick(MakeTick(7205, 7215));  // Move one tick up.
  h.strat.onMarketData();
  auto& sent = h.impl.test_sent();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].type, 'R');
  EXPECT_EQ(sent[1].type, 'R');
}

// Three consecutive rejects on a side disables that side.
TEST(IrageStrat, RejectBackoffDisablesSide) {
  Harness h;
  h.impl.test_setRunning(true);
  EXEC_REPORT rej;
  rej.securityId = kSym;
  rej.orderSide = SIDE_BUY;
  rej.rejCode = 16418;
  h.strat.onRej(rej);
  h.strat.onRej(rej);
  h.strat.onRej(rej);
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  auto& sent = h.impl.test_sent();
  ASSERT_EQ(sent.size(), 1u);  // Only the ask survives.
  EXPECT_EQ(sent[0].quote.side, SIDE_SELL);
}

// Kill switch via UI update cancels everything and stops quoting.
TEST(IrageStrat, KillSwitchCancelsAll) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  h.impl.test_ackAll();
  h.impl.test_sent().clear();

  std::map<int, std::string> upd;
  upd[static_cast<int>(ui_flids::kKillSwitch)] = "1";
  h.strat.update(upd);
  h.strat.onMarketData();
  auto& sent = h.impl.test_sent();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].type, 'X');
  EXPECT_EQ(sent[1].type, 'X');
  // And nothing new after the cancels ack.
  h.impl.test_ackAll();
  h.impl.test_sent().clear();
  h.strat.onMarketData();
  EXPECT_TRUE(h.impl.test_sent().empty());
}

// From 15:25 IST the strategy cancels and stops quoting (pre-close sweep).
TEST(IrageStrat, PreCloseSweep) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  h.impl.test_ackAll();
  h.impl.test_sent().clear();

  h.impl.test_setSeconds(kUtc1526Ist);
  h.strat.onMarketData();
  auto& sent = h.impl.test_sent();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].type, 'X');
  EXPECT_EQ(sent[1].type, 'X');
}

// At max long inventory only the sell side is quoted.
TEST(IrageStrat, MaxLongOnlyAsk) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setInventory(kSym, 150);  // == max (10 lots * 15).
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  auto& sent = h.impl.test_sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].quote.side, SIDE_SELL);
}

// OPS above the headroom threshold: no re-quoting.
TEST(IrageStrat, OpsGuardStopsQuoting) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setOps(90, 100);  // 90% >= 80% threshold.
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  EXPECT_TRUE(h.impl.test_sent().empty());
}

// DPR clamp: an out-of-range ask is snapped to the band.
TEST(IrageStrat, DprClampsQuotes) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setDpr(kSym, 7000, 7210);  // Ask 7215 would breach the DPR high.
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();
  auto& sent = h.impl.test_sent();
  ASSERT_EQ(sent.size(), 2u);
  for (size_t i = 0; i < sent.size(); ++i) {
    EXPECT_LE(sent[i].quote.price, 7210);
    EXPECT_GE(sent[i].quote.price, 7000);
  }
}

// Fills update cash and the UI snapshot.
TEST(IrageStrat, ExecUpdatesPnlAndUi) {
  Harness h;
  h.impl.test_setRunning(true);
  h.impl.test_setMtick(MakeTick(7200, 7210));
  h.strat.onMarketData();

  EXEC_REPORT fill;
  fill.securityId = kSym;
  fill.orderSide = SIDE_SELL;
  fill.price = 7215;
  fill.fillQty = 15;
  h.strat.onExec(fill);
  EXPECT_EQ(h.strat.fills.load(), 1);
  EXPECT_EQ(h.strat.cash_paise.load(), 15LL * 7215);

  h.impl.test_setInventory(kSym, -15);
  std::ostringstream oss;
  h.strat.getUIData(kSym, oss);
  const std::string ui = oss.str();
  EXPECT_NE(ui.find("9810=-15"), std::string::npos);  // Position flid.
  EXPECT_NE(ui.find("9813=1"), std::string::npos);    // Fills flid.
}

}  // namespace
}  // namespace my_strat_ns
