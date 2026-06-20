// A single price level: the aggregate of every resting order queued at one
// price. Levels are stored by value inside flat, sorted arrays in OrderBook --
// there is no per-order linked list (cache-unfriendly) at this layer.
#ifndef HFT_ORDER_BOOK_PRICE_LEVEL_HPP_
#define HFT_ORDER_BOOK_PRICE_LEVEL_HPP_

#include "common/compiler.hpp"
#include "common/types.hpp"

namespace hft {

struct PriceLevel {
  Price price = 0;             // In paisa.
  Quantity total_quantity = 0; // Sum of all resting order quantities here.
  int32_t order_count = 0;     // Number of resting orders at this price.

  HFT_ALWAYS_INLINE void add_order(Quantity qty) {
    total_quantity += qty;
    ++order_count;
  }

  // Reduce an order's contribution (cancel or partial trade). When the last
  // order leaves, order_count reaches zero and the level can be removed.
  HFT_ALWAYS_INLINE void remove_order(Quantity qty, bool drop_order) {
    total_quantity -= qty;
    if (total_quantity < 0) {
      total_quantity = 0;
    }
    if (drop_order && order_count > 0) {
      --order_count;
    }
  }

  // Adjust the resting quantity in place (a modify that keeps the same price).
  HFT_ALWAYS_INLINE void modify_order(Quantity old_qty, Quantity new_qty) {
    total_quantity += (new_qty - old_qty);
    if (total_quantity < 0) {
      total_quantity = 0;
    }
  }

  HFT_ALWAYS_INLINE bool empty() const { return order_count <= 0; }
};

}  // namespace hft

#endif  // HFT_ORDER_BOOK_PRICE_LEVEL_HPP_
