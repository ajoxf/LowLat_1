// Fill simulator for offline backtesting.
//
// Models how our resting quotes would have been filled against the historical
// (or synthetic) market. The market-making strategy keeps at most one live
// quote per side per token, so we track exactly that. A trade printing through
// our price fills us at our (passive) limit price.
//
// This is an OPTIMISTIC model: it ignores queue position (it assumes we are at
// the front of the queue at our price). A conservative backtest would only fill
// us after enough volume traded ahead of us; that refinement is noted below.
#ifndef HFT_SIM_FILL_SIMULATOR_HPP_
#define HFT_SIM_FILL_SIMULATOR_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/types.hpp"

namespace hft {

inline constexpr size_t kSimMaxTokens = 65536;

class FillSimulator {
 public:
  FillSimulator() : bid_(kSimMaxTokens), ask_(kSimMaxTokens) {}

  // Record (or replace) our current resting quote on a side. The strategy
  // cancels-and-replaces, so a new quote supersedes the previous one.
  void register_order(const Order& o) {
    if (o.token >= kSimMaxTokens) {
      return;
    }
    SimQuote q{true, o.price, o.qty, o.client_order_id, o.token};
    (o.side == kBuy ? bid_ : ask_)[o.token] = q;
  }

  // Process a market event against our resting quotes, emitting a FillReport
  // for each fill. Only trades cause fills here.
  template <typename EmitFn>
  void on_event(const MarketEvent& ev, EmitFn&& emit) {
    if (static_cast<MtbtMsgType>(ev.type) != MtbtMsgType::kTrade) {
      return;
    }
    if (ev.token >= kSimMaxTokens) {
      return;
    }
    const Price p = ev.price;

    // Our resting BUY fills when a trade prints at or below our bid price
    // (the market came down to us). A more conservative model would require
    // ev.qty to exceed the volume resting ahead of us at that price.
    SimQuote& b = bid_[ev.token];
    if (b.active && p <= b.price) {
      emit(MakeFill(b, kBuy));
      b.active = false;
      ++fills_;
    }
    // Our resting SELL fills when a trade prints at or above our ask price.
    SimQuote& a = ask_[ev.token];
    if (a.active && p >= a.price) {
      emit(MakeFill(a, kSell));
      a.active = false;
      ++fills_;
    }
  }

  uint64_t fills() const { return fills_; }

 private:
  struct SimQuote {
    bool active = false;
    Price price = 0;
    Quantity qty = 0;
    ClOrdId id = 0;
    Token token = 0;
  };

  FillReport MakeFill(const SimQuote& q, Side side) {
    FillReport f{};
    f.client_order_id = q.id;
    f.token = q.token;
    f.side = side;
    f.fill_price = q.price;  // Passive: we get our limit price.
    f.fill_qty = q.qty;
    f.remaining_qty = 0;
    f.trade_no = next_trade_no_++;
    return f;
  }

  std::vector<SimQuote> bid_;  // Per-token current resting bid.
  std::vector<SimQuote> ask_;  // Per-token current resting ask.
  uint64_t fills_ = 0;
  int64_t next_trade_no_ = 1;
};

}  // namespace hft

#endif  // HFT_SIM_FILL_SIMULATOR_HPP_
