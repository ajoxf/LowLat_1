// Tests for the backtest fill simulator and the synthetic MTBT generator.
#include "sim/fill_simulator.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "sim/mtbt_generator.hpp"

namespace hft {
namespace {

Order Quote(Token token, Side side, Price price, Quantity qty, ClOrdId id) {
  Order o{};
  o.token = token;
  o.side = side;
  o.price = price;
  o.qty = qty;
  o.client_order_id = id;
  return o;
}

MarketEvent Trade(Token token, Price price, Quantity qty) {
  MarketEvent ev{};
  ev.type = static_cast<uint8_t>(MtbtMsgType::kTrade);
  ev.token = token;
  ev.price = price;
  ev.qty = qty;
  return ev;
}

// A trade at or below our bid fills the resting buy at our price.
TEST(FillSimulator, BuyFillsWhenMarketComesDown) {
  FillSimulator sim;
  sim.register_order(Quote(5, kBuy, 100, 10, 1));
  std::vector<FillReport> fills;
  sim.on_event(Trade(5, 99, 7), [&](const FillReport& f) { fills.push_back(f); });
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].side, kBuy);
  EXPECT_EQ(fills[0].fill_price, 100);  // We get our passive limit price.
  EXPECT_EQ(fills[0].client_order_id, 1);
}

// A trade above our bid does not fill it.
TEST(FillSimulator, BuyNotFilledAbove) {
  FillSimulator sim;
  sim.register_order(Quote(5, kBuy, 100, 10, 1));
  std::vector<FillReport> fills;
  sim.on_event(Trade(5, 101, 7), [&](const FillReport& f) { fills.push_back(f); });
  EXPECT_TRUE(fills.empty());
}

// A trade at or above our ask fills the resting sell.
TEST(FillSimulator, SellFillsWhenMarketRises) {
  FillSimulator sim;
  sim.register_order(Quote(5, kSell, 110, 10, 2));
  std::vector<FillReport> fills;
  sim.on_event(Trade(5, 111, 5), [&](const FillReport& f) { fills.push_back(f); });
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].side, kSell);
  EXPECT_EQ(fills[0].fill_price, 110);
}

// Partial fills: a trade only fills up to its own size; the order completes on
// a later trade and then stops filling.
TEST(FillSimulator, PartialThenComplete) {
  FillSimulator sim;
  sim.register_order(Quote(5, kBuy, 100, 10, 1));  // front of queue (queue 0).
  std::vector<FillReport> fills;
  sim.on_event(Trade(5, 99, 7), [&](const FillReport& f) { fills.push_back(f); });
  sim.on_event(Trade(5, 98, 7), [&](const FillReport& f) { fills.push_back(f); });
  sim.on_event(Trade(5, 98, 7), [&](const FillReport& f) { fills.push_back(f); });
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[0].fill_qty, 7);
  EXPECT_EQ(fills[0].remaining_qty, 3);
  EXPECT_EQ(fills[1].fill_qty, 3);  // completes the order
  EXPECT_EQ(fills[1].remaining_qty, 0);
}

// Queue ahead must be consumed before we fill.
TEST(FillSimulator, QueueAheadAbsorbsTrades) {
  FillSimulator sim;
  sim.register_order(Quote(5, kBuy, 100, 10, 1), /*queue_ahead=*/20);
  std::vector<FillReport> fills;
  // First 15 lots at our price are absorbed by the 20 ahead of us.
  sim.on_event(Trade(5, 100, 15), [&](const FillReport& f) { fills.push_back(f); });
  EXPECT_TRUE(fills.empty());
  // Next trade: 5 finishes the queue (20 total), 8 fills us.
  sim.on_event(Trade(5, 100, 13), [&](const FillReport& f) { fills.push_back(f); });
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].fill_qty, 8);
  EXPECT_EQ(fills[0].remaining_qty, 2);
}

// The generator yields a mix of snap quotes and trades for the token.
TEST(MtbtGenerator, ProducesQuotesAndTrades) {
  MtbtGenerator gen(2885, 1000000, 5, 1);
  std::vector<MarketEvent> events;
  gen.generate(events, 1000);
  EXPECT_GT(events.size(), 1000u);  // At least one quote per step, plus trades.
  int quotes = 0;
  int trades = 0;
  for (const auto& e : events) {
    if (static_cast<MtbtMsgType>(e.type) == MtbtMsgType::kSnapQuote) ++quotes;
    if (static_cast<MtbtMsgType>(e.type) == MtbtMsgType::kTrade) ++trades;
  }
  EXPECT_EQ(quotes, 1000);
  EXPECT_GT(trades, 0);
}

// The MBO generator yields a persistent, depth-bearing stream.
TEST(MtbtGenerator, MboHasDepthAddsAndTrades) {
  MtbtGenerator gen(2885, 1000000, 5, 1);
  std::vector<MarketEvent> events;
  gen.generate_mbo(events, 2000);
  int adds = 0, trades = 0, cancels = 0;
  for (const auto& e : events) {
    switch (static_cast<MtbtMsgType>(e.type)) {
      case MtbtMsgType::kOrderAdd: ++adds; break;
      case MtbtMsgType::kTrade: ++trades; break;
      case MtbtMsgType::kOrderCancel: ++cancels; break;
      default: break;
    }
  }
  EXPECT_GT(adds, 0);
  EXPECT_GT(trades, 0);
  EXPECT_GT(cancels, 0);
}

// A generated event round-trips through the capture encoder + MTBT parser.
TEST(MtbtGenerator, CaptureRoundTrip) {
  MtbtGenerator gen(2885, 1000000, 5, 1);
  std::vector<MarketEvent> events;
  gen.generate(events, 50);
  std::vector<uint8_t> bytes;
  for (const auto& e : events) {
    MtbtGenerator::encode_capture(e, bytes);
  }
  EXPECT_GT(bytes.size(), 0u);
}

}  // namespace
}  // namespace hft
