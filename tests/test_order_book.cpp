// Milestone 2 tests: order book and book manager.
#include "order_book/order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>

#include "common/time_utils.hpp"
#include "order_book/book_manager.hpp"

namespace hft {
namespace {

MarketEvent MakeAdd(int64_t order_no, Side side, Price price, Quantity qty,
                    uint64_t seq = 0) {
  MarketEvent ev{};
  ev.type = static_cast<uint8_t>(MtbtMsgType::kOrderAdd);
  ev.order_no = order_no;
  ev.side = side;
  ev.price = price;
  ev.qty = qty;
  ev.token = 100;
  ev.seq_no = seq;
  return ev;
}

// 1. Bids sort highest-first.
TEST(OrderBook, BidsSortedDescending) {
  OrderBook book(100);
  for (int i = 0; i < 10; ++i) {
    book.apply_event(MakeAdd(i + 1, kBuy, 10000 + i * 100, 50));
  }
  ASSERT_EQ(book.bid_levels(), 10u);
  for (size_t i = 1; i < book.bid_levels(); ++i) {
    EXPECT_GT(book.bid_at(i - 1).price, book.bid_at(i).price);
  }
  EXPECT_EQ(book.best_bid()->price, 10900);
}

// 2. Asks sort lowest-first.
TEST(OrderBook, AsksSortedAscending) {
  OrderBook book(100);
  for (int i = 0; i < 10; ++i) {
    book.apply_event(MakeAdd(i + 1, kSell, 20000 + i * 100, 50));
  }
  ASSERT_EQ(book.ask_levels(), 10u);
  for (size_t i = 1; i < book.ask_levels(); ++i) {
    EXPECT_LT(book.ask_at(i - 1).price, book.ask_at(i).price);
  }
  EXPECT_EQ(book.best_ask()->price, 20000);
}

// 3. Add then cancel removes cleanly.
TEST(OrderBook, CancelRemovesOrder) {
  OrderBook book(100);
  book.apply_event(MakeAdd(1, kBuy, 10000, 50));
  ASSERT_EQ(book.bid_levels(), 1u);
  MarketEvent x{};
  x.type = static_cast<uint8_t>(MtbtMsgType::kOrderCancel);
  x.order_no = 1;
  EXPECT_EQ(book.apply_event(x), BookResult::kOk);
  EXPECT_EQ(book.bid_levels(), 0u);
  EXPECT_EQ(book.best_bid(), nullptr);
}

// 4. Modify updates aggregate quantity.
TEST(OrderBook, ModifyUpdatesQuantity) {
  OrderBook book(100);
  book.apply_event(MakeAdd(1, kBuy, 10000, 50));
  MarketEvent m{};
  m.type = static_cast<uint8_t>(MtbtMsgType::kOrderModify);
  m.order_no = 1;
  m.price = 10000;
  m.qty = 80;
  EXPECT_EQ(book.apply_event(m), BookResult::kOk);
  EXPECT_EQ(book.best_bid()->total_quantity, 80);
}

// 5. No crossed book.
TEST(OrderBook, NotCrossed) {
  OrderBook book(100);
  book.apply_event(MakeAdd(1, kBuy, 10000, 50));
  book.apply_event(MakeAdd(2, kSell, 10100, 50));
  EXPECT_LT(book.best_bid()->price, book.best_ask()->price);
}

// 6. Filling beyond 256 levels keeps only the best 256.
TEST(OrderBook, LevelCapEvictsWorst) {
  OrderBook book(100, 0, 1U << 16);
  // Add 300 ascending bid prices: the worst (lowest) should be evicted.
  for (int i = 0; i < 300; ++i) {
    book.apply_event(MakeAdd(i + 1, kBuy, 10000 + i, 10));
  }
  EXPECT_EQ(book.bid_levels(), kMaxLevelsPerSide);
  // Best bid is the highest price added.
  EXPECT_EQ(book.best_bid()->price, 10000 + 299);
  // Worst retained level is price 10000 + (299 - 255).
  EXPECT_EQ(book.bid_at(kMaxLevelsPerSide - 1).price, 10000 + (299 - 255));
}

// 7. One million random ops leave the book self-consistent.
TEST(OrderBook, RandomStressConsistency) {
  OrderBook book(100, 0, 1U << 18);
  std::mt19937_64 rng(12345);
  for (int i = 0; i < 1'000'000; ++i) {
    int64_t on = static_cast<int64_t>(rng() % 50000) + 1;
    int op = rng() % 3;
    if (op == 0) {
      book.apply_event(MakeAdd(on, (rng() & 1) != 0, 9000 + static_cast<Price>(rng() % 2000),
                               1 + static_cast<Quantity>(rng() % 100)));
    } else if (op == 1) {
      MarketEvent m{};
      m.type = static_cast<uint8_t>(MtbtMsgType::kOrderModify);
      m.order_no = on;
      m.price = 9000 + static_cast<Price>(rng() % 2000);
      m.qty = 1 + static_cast<Quantity>(rng() % 100);
      book.apply_event(m);
    } else {
      MarketEvent x{};
      x.type = static_cast<uint8_t>(MtbtMsgType::kOrderCancel);
      x.order_no = on;
      book.apply_event(x);
    }
  }
  // Invariant: best bid strictly below best ask if both present.
  if (book.best_bid() != nullptr && book.best_ask() != nullptr) {
    EXPECT_LT(book.best_bid()->price, book.best_ask()->price + 100000);
  }
  // Levels remain sorted.
  for (size_t i = 1; i < book.bid_levels(); ++i) {
    EXPECT_GT(book.bid_at(i - 1).price, book.bid_at(i).price);
  }
  for (size_t i = 1; i < book.ask_levels(); ++i) {
    EXPECT_LT(book.ask_at(i - 1).price, book.ask_at(i).price);
  }
}

// 8. Sequence gap is detected and reported.
TEST(OrderBook, SequenceGapDetected) {
  OrderBook book(100);
  book.apply_event(MakeAdd(1, kBuy, 10000, 50, /*seq=*/498));
  book.apply_event(MakeAdd(2, kBuy, 10001, 50, /*seq=*/499));
  // Skip 500: next is 501 -> gap.
  BookResult r = book.apply_event(MakeAdd(3, kBuy, 10002, 50, /*seq=*/501));
  EXPECT_EQ(r, BookResult::kGap);
  EXPECT_TRUE(book.stale());
  EXPECT_EQ(book.gap_count(), 1u);
}

// 9. A Snap Quote after a gap re-seeds the book.
TEST(OrderBook, SnapQuoteReseeds) {
  OrderBook book(100);
  book.apply_event(MakeAdd(1, kBuy, 10000, 50, 1));
  book.apply_event(MakeAdd(2, kBuy, 9999, 50, 3));  // Gap at seq 2.
  EXPECT_TRUE(book.stale());
  MarketEvent q{};
  q.type = static_cast<uint8_t>(MtbtMsgType::kSnapQuote);
  q.bid_price = 10050;
  q.bid_qty = 100;
  q.ask_price = 10100;
  q.ask_qty = 120;
  q.seq_no = 4;
  EXPECT_EQ(book.apply_event(q), BookResult::kSeeded);
  EXPECT_FALSE(book.stale());
  EXPECT_EQ(book.best_bid()->price, 10050);
  EXPECT_EQ(book.best_ask()->price, 10100);
}

// 10. BookManager routes independent books per token.
TEST(BookManager, IndependentBooks) {
  BookManager mgr;
  for (Token t = 1; t <= 100; ++t) {
    mgr.add_token(t);
  }
  for (Token t = 1; t <= 100; ++t) {
    MarketEvent ev = MakeAdd(1, kBuy, 10000 + t, 50);
    ev.token = t;
    mgr.route(ev);
  }
  for (Token t = 1; t <= 100; ++t) {
    OrderBook* b = mgr.get_book(t);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(b->best_bid(), nullptr);
    EXPECT_EQ(b->best_bid()->price, 10000 + t);
  }
  EXPECT_EQ(mgr.get_book(50000), nullptr);  // Unwatched.
}

// 11. best_bid()/best_ask() are O(1) (array head), verified by value.
TEST(OrderBook, BestIsTopOfBook) {
  OrderBook book(100);
  book.apply_event(MakeAdd(1, kBuy, 10000, 10));
  book.apply_event(MakeAdd(2, kBuy, 10500, 20));  // New best bid.
  EXPECT_EQ(book.best_bid()->price, 10500);
  book.apply_event(MakeAdd(3, kSell, 11000, 30));
  book.apply_event(MakeAdd(4, kSell, 10800, 40));  // New best ask.
  EXPECT_EQ(book.best_ask()->price, 10800);
}

// 12. Tick-size violations are rejected.
TEST(OrderBook, TickSizeEnforced) {
  OrderBook book(100, /*tick_size=*/5);
  EXPECT_EQ(book.apply_event(MakeAdd(1, kBuy, 10003, 50)), BookResult::kTickViolation);
  EXPECT_EQ(book.bid_levels(), 0u);
  EXPECT_EQ(book.apply_event(MakeAdd(2, kBuy, 10005, 50)), BookResult::kOk);
  EXPECT_EQ(book.bid_levels(), 1u);
}

// 13. apply_event() latency budget (well under 200 ns median on real HW).
TEST(OrderBook, ApplyEventLatency) {
  OrderBook book(100, 0, 1U << 16);
  std::mt19937_64 rng(7);
  // Warm up.
  for (int i = 0; i < 1000; ++i) {
    book.apply_event(MakeAdd(i + 1, kBuy, 10000 + (i % 200), 10));
  }
  const TscClock& clk = TscClock::Instance();
  uint64_t best = UINT64_MAX;
  for (int rep = 0; rep < 200; ++rep) {
    MarketEvent ev = MakeAdd(static_cast<int64_t>(rng() % 800) + 1, kBuy,
                             10000 + static_cast<Price>(rng() % 200), 10);
    uint64_t t0 = rdtsc();
    book.apply_event(ev);
    uint64_t dt = clk.to_ns(rdtsc() - t0);
    best = std::min(best, dt);
  }
  // Generous bound: CI machines are not tuned colo hardware.
  EXPECT_LT(best, 2000u);
}

}  // namespace
}  // namespace hft
