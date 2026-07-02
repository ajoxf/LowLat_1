// Shared market-making quoting math.
//
// Pure, allocation-free integer functions used by BOTH execution paths:
//   * the self-hosted stack (src/strategy/market_making.hpp), and
//   * the iRage Connect adapter (irage/mystrat.cpp).
// Keep this header C++17-compatible (no spans/concepts/designated inits):
// the iRage toolchain compiles strategies with -std=c++1z.
//
// All prices are integer paise, all skews are integer ticks. No floating point.
#ifndef HFT_STRATEGY_QUOTING_MATH_HPP_
#define HFT_STRATEGY_QUOTING_MATH_HPP_

#include <cstdint>

namespace hft {
namespace quoting {

inline int clamp_ticks(int v, int lim) { return v > lim ? lim : (v < -lim ? -lim : v); }

// Inventory skew: lean AGAINST the position so quotes flatten it.
// Long (positive position) -> positive skew -> caller shifts quotes DOWN.
// Proportional: position/max_position scaled to max_skew_ticks, clamped.
inline int inventory_skew_ticks(int64_t position_units, int64_t max_position_units,
                                int max_skew_ticks) {
  if (max_skew_ticks <= 0 || max_position_units <= 0) {
    return 0;
  }
  return clamp_ticks(
      static_cast<int>(position_units * max_skew_ticks / max_position_units),
      max_skew_ticks);
}

// Order-flow imbalance skew: lean WITH top-of-book pressure.
// Bid-heavy book (buy pressure) -> positive skew -> caller shifts quotes UP.
// imbalance = (bid_qty - ask_qty) / (bid_qty + ask_qty), scaled to ticks.
inline int flow_skew_ticks(int64_t bid_qty, int64_t ask_qty, int max_skew_ticks) {
  if (max_skew_ticks <= 0) {
    return 0;
  }
  const int64_t denom = bid_qty + ask_qty;
  if (denom <= 0) {
    return 0;
  }
  return clamp_ticks(static_cast<int>((bid_qty - ask_qty) * max_skew_ticks / denom),
                     max_skew_ticks);
}

// Combined desired quote pair: one tick inside the touch, shifted by
// (flow - inventory) ticks. flow pushes with the predicted move; inventory
// pushes toward flat. With both skews 0 this is the naive quote.
struct DesiredQuotes {
  int64_t bid_price;
  int64_t ask_price;
};

inline DesiredQuotes desired_quotes(int64_t best_bid, int64_t best_ask, int64_t tick,
                                    int64_t bid_qty, int64_t ask_qty,
                                    int64_t position_units, int64_t max_position_units,
                                    int max_inv_skew_ticks, int max_flow_skew_ticks) {
  const int inv = inventory_skew_ticks(position_units, max_position_units, max_inv_skew_ticks);
  const int flow = flow_skew_ticks(bid_qty, ask_qty, max_flow_skew_ticks);
  const int64_t shift = static_cast<int64_t>(flow - inv) * tick;
  DesiredQuotes q;
  q.bid_price = (best_bid - tick) + shift;
  q.ask_price = (best_ask + tick) + shift;
  return q;
}

}  // namespace quoting
}  // namespace hft

#endif  // HFT_STRATEGY_QUOTING_MATH_HPP_
