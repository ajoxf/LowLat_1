// Owns one OrderBook per watched NSE token and routes MarketEvents to the
// correct book in O(1) via a flat array indexed directly by token value (NSE
// tokens are < 65536, so no hashing is required).
#ifndef HFT_ORDER_BOOK_BOOK_MANAGER_HPP_
#define HFT_ORDER_BOOK_BOOK_MANAGER_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/compiler.hpp"
#include "common/types.hpp"
#include "order_book/order_book.hpp"

namespace hft {

inline constexpr size_t kMaxTokens = 65536;

class BookManager {
 public:
  BookManager() : slots_(kMaxTokens), watched_(0) {}

  // Register a token to be tracked. Pre-allocates its OrderBook now (startup),
  // so the hot path never allocates. Safe to call again to update tick size.
  OrderBook* add_token(Token token, Price tick_size = 0, size_t order_capacity = 1U << 14) {
    if (token >= kMaxTokens) {
      return nullptr;
    }
    if (!slots_[token]) {
      slots_[token] = std::make_unique<OrderBook>(token, tick_size, order_capacity);
      ++watched_;
    } else {
      slots_[token]->set_tick_size(tick_size);
    }
    return slots_[token].get();
  }

  // O(1) lookup; nullptr if the token is not watched.
  HFT_ALWAYS_INLINE OrderBook* get_book(Token token) {
    if (HFT_UNLIKELY(token >= kMaxTokens)) {
      return nullptr;
    }
    return slots_[token].get();
  }

  // Route a market event to its book. Events for unwatched tokens are skipped
  // (most of NSE's contracts are not in our watch list).
  HFT_ALWAYS_INLINE BookResult route(const MarketEvent& ev) {
    OrderBook* book = get_book(ev.token);
    if (HFT_UNLIKELY(book == nullptr)) {
      return BookResult::kOk;  // Not watched; ignore.
    }
    return book->apply_event(ev);
  }

  size_t watched_count() const { return watched_; }

 private:
  // unique_ptr per slot keeps the (non-trivial, heavy) OrderBook objects out of
  // a single giant contiguous allocation while preserving O(1) token indexing.
  std::vector<std::unique_ptr<OrderBook>> slots_;
  size_t watched_;
};

}  // namespace hft

#endif  // HFT_ORDER_BOOK_BOOK_MANAGER_HPP_
