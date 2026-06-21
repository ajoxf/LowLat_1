// Fill simulator for offline backtesting.
//
// Models how our resting quotes would have been filled against the historical
// (or synthetic) market. The market-making strategy keeps at most one live
// quote per side per token, so we track exactly that.
//
// Queue-aware model: when we join a price level, `queue_ahead` orders sit in
// front of us (resting volume already displayed there). A trade printing at or
// through our price first consumes the queue ahead of us, and only the residual
// fills our order -- which naturally produces partial fills and the lower fill
// rate a real maker experiences. With queue_ahead = 0 this degrades to the
// optimistic front-of-queue model.
#ifndef HFT_SIM_FILL_SIMULATOR_HPP_
#define HFT_SIM_FILL_SIMULATOR_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/types.hpp"

namespace hft {

inline constexpr size_t kSimMaxTokens = 65536;

class FillSimulator {
 public:
  FillSimulator() : bid_(kSimMaxTokens), ask_(kSimMaxTokens) {}

  // Record (or replace) our current resting quote on a side. `queue_ahead` is
  // the volume already resting at our price when we join (0 = front of queue).
  // The strategy cancels-and-replaces, so a new quote supersedes the previous.
  void register_order(const Order& o, Quantity queue_ahead = 0) {
    if (o.token >= kSimMaxTokens) {
      return;
    }
    SimQuote q{true, o.price, o.qty, queue_ahead < 0 ? 0 : queue_ahead, o.client_order_id,
               o.token};
    (o.side == kBuy ? bid_ : ask_)[o.token] = q;
  }

  // Process a market event against our resting quotes, emitting a FillReport for
  // each (possibly partial) fill. Only trades cause fills.
  template <typename EmitFn>
  void on_event(const MarketEvent& ev, EmitFn&& emit) {
    if (static_cast<MtbtMsgType>(ev.type) != MtbtMsgType::kTrade) {
      return;
    }
    if (ev.token >= kSimMaxTokens) {
      return;
    }
    const Price p = ev.price;
    Quantity trade_qty = ev.qty;

    // Our resting BUY fills when a trade prints at or below our bid price.
    SimQuote& b = bid_[ev.token];
    if (b.active && p <= b.price) {
      MatchAgainst(b, kBuy, trade_qty, emit);
    }
    // Our resting SELL fills when a trade prints at or above our ask price.
    SimQuote& a = ask_[ev.token];
    if (a.active && p >= a.price) {
      MatchAgainst(a, kSell, trade_qty, emit);
    }
  }

  uint64_t fills() const { return fills_; }

 private:
  struct SimQuote {
    bool active = false;
    Price price = 0;
    Quantity remaining = 0;
    Quantity queue_ahead = 0;
    ClOrdId id = 0;
    Token token = 0;
  };

  template <typename EmitFn>
  void MatchAgainst(SimQuote& q, Side side, Quantity trade_qty, EmitFn&& emit) {
    // The trade first consumes the queue ahead of us.
    const Quantity eat_queue = std::min(q.queue_ahead, trade_qty);
    q.queue_ahead -= eat_queue;
    trade_qty -= eat_queue;
    if (trade_qty <= 0) {
      return;  // The trade was entirely absorbed by the queue ahead of us.
    }
    // The residual fills (part of) our order.
    const Quantity fill_qty = std::min(q.remaining, trade_qty);
    if (fill_qty <= 0) {
      return;
    }
    q.remaining -= fill_qty;
    ++fills_;
    FillReport f{};
    f.client_order_id = q.id;
    f.token = q.token;
    f.side = side;
    f.fill_price = q.price;  // Passive: we get our limit price.
    f.fill_qty = fill_qty;
    f.remaining_qty = q.remaining;
    f.trade_no = next_trade_no_++;
    emit(f);
    if (q.remaining <= 0) {
      q.active = false;
    }
  }

  std::vector<SimQuote> bid_;  // Per-token current resting bid.
  std::vector<SimQuote> ask_;  // Per-token current resting ask.
  uint64_t fills_ = 0;
  int64_t next_trade_no_ = 1;
};

}  // namespace hft

#endif  // HFT_SIM_FILL_SIMULATOR_HPP_
