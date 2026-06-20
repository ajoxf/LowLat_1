// Benchmark: OrderBook::apply_event() throughput under mixed ADD/MODIFY/CANCEL.
#include "order_book/order_book.hpp"

#include <benchmark/benchmark.h>

#include <random>

#include "common/types.hpp"

namespace {

hft::MarketEvent MakeAdd(int64_t no, bool buy, hft::Price price, hft::Quantity qty) {
  hft::MarketEvent ev{};
  ev.type = static_cast<uint8_t>(hft::MtbtMsgType::kOrderAdd);
  ev.order_no = no;
  ev.side = buy;
  ev.price = price;
  ev.qty = qty;
  ev.token = 100;
  return ev;
}

void BM_OrderBookApplyAdd(benchmark::State& state) {
  hft::OrderBook book(100, 0, 1U << 18);
  int64_t no = 1;
  std::mt19937_64 rng(1);
  for (auto _ : state) {
    auto ev = MakeAdd(no++, (rng() & 1) != 0, 10000 + static_cast<hft::Price>(rng() % 200), 10);
    book.apply_event(ev);
    benchmark::DoNotOptimize(book.best_bid());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookApplyAdd);

void BM_OrderBookMixed(benchmark::State& state) {
  hft::OrderBook book(100, 0, 1U << 18);
  std::mt19937_64 rng(7);
  for (auto _ : state) {
    int64_t on = static_cast<int64_t>(rng() % 60000) + 1;
    int op = rng() % 3;
    hft::MarketEvent ev{};
    ev.token = 100;
    ev.order_no = on;
    if (op == 0) {
      ev = MakeAdd(on, (rng() & 1) != 0, 9000 + static_cast<hft::Price>(rng() % 2000), 10);
    } else if (op == 1) {
      ev.type = static_cast<uint8_t>(hft::MtbtMsgType::kOrderModify);
      ev.price = 9000 + static_cast<hft::Price>(rng() % 2000);
      ev.qty = 20;
    } else {
      ev.type = static_cast<uint8_t>(hft::MtbtMsgType::kOrderCancel);
    }
    book.apply_event(ev);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookMixed);

}  // namespace

BENCHMARK_MAIN();
