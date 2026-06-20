// Tick-to-trade pipeline: wires the order book, strategy, risk manager and
// order manager into a single hot path and measures end-to-end latency.
//
//   T0  raw MTBT event enters the pipeline (NIC / recvmmsg boundary)
//   T2  OrderBook::apply_event() complete
//   T3  Strategy::on_book_update() generated order(s)
//   T4  RiskManager::check() approved
//   T5  NNF message encoded and handed to the order gateway (write boundary)
//
// The same component is used by main.cpp, the latency test and the pipeline
// benchmark so they all measure the identical code path.
#ifndef HFT_PIPELINE_HPP_
#define HFT_PIPELINE_HPP_

#include <cstddef>
#include <cstdint>
#include <span>

#include "common/time_utils.hpp"
#include "common/types.hpp"
#include "order_book/book_manager.hpp"
#include "order_manager/nnf_encoder.hpp"
#include "order_manager/order_manager.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/market_making.hpp"

namespace hft {

struct PipelineStats {
  uint64_t events = 0;
  uint64_t orders_generated = 0;
  uint64_t orders_approved = 0;
  uint64_t orders_rejected = 0;
  uint64_t orders_sent = 0;
};

class Pipeline {
 public:
  Pipeline(BookManager* books, MarketMaking* strat, RiskManager* risk, OrderManager* om)
      : books_(books),
        strat_(strat),
        risk_(risk),
        om_(om),
        tick_to_trade_(1'000'000) {}

  // Drive both the strategy and risk simulated clocks (test/replay use).
  void set_now(Timestamp ist_ns) {
    now_ns_ = ist_ns;
    strat_->set_now(ist_ns);
    risk_->set_now(ist_ns);
  }

  // Process one decoded market event end-to-end. Returns the number of orders
  // sent for this event. Records tick-to-trade latency whenever an order is
  // produced.
  size_t process(const MarketEvent& ev) {
    const uint64_t t0 = rdtsc();
    ++stats_.events;

    OrderBook* book = books_->get_book(ev.token);
    if (book == nullptr) {
      return 0;  // Unwatched token.
    }
    book->apply_event(ev);

    // Keep risk's price reference current from top of book.
    const PriceLevel* bb = book->best_bid();
    const PriceLevel* ba = book->best_ask();
    risk_->update_reference(ev.token, bb != nullptr ? bb->price : 0,
                            ba != nullptr ? ba->price : 0);

    strat_->on_book_update(ev.token, *book);
    auto orders = strat_->get_pending_orders();
    if (orders.empty()) {
      return 0;
    }
    stats_.orders_generated += orders.size();

    size_t sent = 0;
    sent_count_ = 0;
    for (const Order& o : orders) {
      Order order = o;
      if (risk_->check(order) != RejectReason::kApproved) {
        ++stats_.orders_rejected;
        continue;
      }
      ++stats_.orders_approved;
      const Timestamp stamp = now_ns_ != 0 ? now_ns_ : ist_now_ns();
      const size_t len = om_->new_order(order, msg_buf_, stamp);
      if (len > 0) {
        ++stats_.orders_sent;
        ++sent;
        last_msg_len_ = len;
        if (sent_count_ < kMaxSent) {
          sent_orders_[sent_count_++] = order;
        }
      }
    }
    if (sent > 0) {
      const uint64_t t5 = rdtsc();
      tick_to_trade_.record(TscClock::Instance().to_ns(t5 - t0));
    }
    return sent;
  }

  // Apply a fill to strategy, risk and order manager.
  void on_fill(const FillReport& fill) {
    strat_->on_fill(fill);
    risk_->on_fill(fill);
    om_->on_fill(fill);
  }

  const PipelineStats& stats() const { return stats_; }
  LatencyTracker& latency() { return tick_to_trade_; }
  size_t last_msg_len() const { return last_msg_len_; }

  // Orders sent during the most recent process() call (for ack/fill simulation
  // in tests and replay tooling).
  std::span<const Order> last_sent() const { return {sent_orders_, sent_count_}; }

 private:
  BookManager* books_;
  MarketMaking* strat_;
  RiskManager* risk_;
  OrderManager* om_;
  LatencyTracker tick_to_trade_;
  PipelineStats stats_;
  Timestamp now_ns_ = 0;
  uint8_t msg_buf_[kNnfMaxMsg];
  size_t last_msg_len_ = 0;

  static constexpr size_t kMaxSent = 16;
  Order sent_orders_[kMaxSent];
  size_t sent_count_ = 0;
};

}  // namespace hft

#endif  // HFT_PIPELINE_HPP_
