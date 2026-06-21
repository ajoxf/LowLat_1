// Per-instrument limit order book maintained from the NSE MTBT (order-by-order)
// feed.
//
// Design choices follow the latency requirements:
//   * Bids and asks are fixed-size, sorted, flat arrays of PriceLevel (no
//     std::map -- no tree traversal, no heap allocation). Bids sort descending,
//     asks ascending, so best_bid()/best_ask() are O(1) array reads.
//   * Lookups use binary search: O(log N); insertions use a bounded shift:
//     O(N) with small, cache-resident N (<= 256).
//   * MTBT cancels/modifies arrive carrying only order_no, so every resting
//     order is tracked in a pre-allocated open-addressing hash table mapping
//     order_no -> {side, price, qty}. No hot-path heap traffic.
#ifndef HFT_ORDER_BOOK_ORDER_BOOK_HPP_
#define HFT_ORDER_BOOK_ORDER_BOOK_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/compiler.hpp"
#include "common/types.hpp"
#include "order_book/price_level.hpp"

namespace hft {

// Outcome of applying a single market event to the book.
enum class BookResult : uint8_t {
  kOk,             // Applied cleanly.
  kSeeded,         // Snap Quote seeded / re-seeded the book.
  kGap,            // Sequence gap detected; book marked stale, awaiting reseed.
  kStaleDropped,   // Event dropped because the book is awaiting a reseed.
  kTickViolation,  // Price not a multiple of tick size; event rejected.
  kUnknownOrder,   // Modify/cancel/trade referenced an untracked order.
  kFull,           // Order table or level array exhausted.
};

inline constexpr size_t kMaxLevelsPerSide = 256;

class OrderBook {
 public:
  // order_capacity must be a power of two; it bounds the number of
  // simultaneously resting orders this book can track.
  explicit OrderBook(Token token = 0, Price tick_size = 0, size_t order_capacity = 1U << 14)
      : token_(token),
        tick_size_(tick_size),
        order_mask_(RoundUpPow2(order_capacity) - 1),
        orders_(order_mask_ + 1),
        bid_count_(0),
        ask_count_(0),
        expected_seq_(0),
        stale_(false),
        gap_count_(0) {}

  Token token() const { return token_; }
  void set_token(Token t) { token_ = t; }
  void set_tick_size(Price t) { tick_size_ = t; }
  Price tick_size() const { return tick_size_; }

  // Single entry point. Routes by event.type (MtbtMsgType). Never throws.
  BookResult apply_event(const MarketEvent& ev) {
    // Per-token sequence-gap detection. seq_no == 0 means "unset / not tracked".
    if (ev.seq_no != 0) {
      if (expected_seq_ != 0 && ev.seq_no != expected_seq_) {
        ++gap_count_;
        stale_ = true;
        expected_seq_ = ev.seq_no + 1;
        // A Snap Quote can still re-seed below; otherwise we drop until reseed.
        if (static_cast<MtbtMsgType>(ev.type) != MtbtMsgType::kSnapQuote) {
          return BookResult::kGap;
        }
      } else {
        expected_seq_ = ev.seq_no + 1;
      }
    }

    switch (static_cast<MtbtMsgType>(ev.type)) {
      case MtbtMsgType::kSnapQuote:
        return Seed(ev);
      case MtbtMsgType::kOrderAdd:
        return stale_ ? BookResult::kStaleDropped : ApplyAdd(ev);
      case MtbtMsgType::kOrderModify:
        return stale_ ? BookResult::kStaleDropped : ApplyModify(ev);
      case MtbtMsgType::kOrderCancel:
        return stale_ ? BookResult::kStaleDropped : ApplyCancel(ev);
      case MtbtMsgType::kTrade:
        return stale_ ? BookResult::kStaleDropped : ApplyTrade(ev);
      default:
        return BookResult::kOk;  // Heartbeat / OI etc.: nothing to do here.
    }
  }

  // O(1) top of book. Returns nullptr when that side is empty.
  HFT_ALWAYS_INLINE const PriceLevel* best_bid() const {
    return bid_count_ > 0 ? &bids_[0] : nullptr;
  }
  HFT_ALWAYS_INLINE const PriceLevel* best_ask() const {
    return ask_count_ > 0 ? &asks_[0] : nullptr;
  }

  size_t bid_levels() const { return bid_count_; }
  size_t ask_levels() const { return ask_count_; }
  const PriceLevel& bid_at(size_t i) const { return bids_[i]; }
  const PriceLevel& ask_at(size_t i) const { return asks_[i]; }

  // Displayed resting quantity at an exact price on the given side (0 if none).
  // Used by the backtest to seed our queue position when we join a level.
  Quantity displayed_qty_at(Side side, Price price) const {
    const PriceLevel* levels = side == kBuy ? bids_ : asks_;
    const size_t count = side == kBuy ? bid_count_ : ask_count_;
    for (size_t i = 0; i < count; ++i) {
      if (levels[i].price == price) {
        return levels[i].total_quantity;
      }
    }
    return 0;
  }

  bool stale() const { return stale_; }
  uint64_t gap_count() const { return gap_count_; }

  // Test / reconnect helper: clear all state.
  void clear() {
    bid_count_ = 0;
    ask_count_ = 0;
    for (auto& slot : orders_) {
      slot.occupied = false;
    }
    stale_ = false;
  }

 private:
  struct OrderRecord {
    int64_t order_no = 0;
    Price price = 0;
    Quantity qty = 0;
    Side side = kBuy;
    bool occupied = false;
  };

  static size_t RoundUpPow2(size_t v) {
    size_t p = 1;
    while (p < v) {
      p <<= 1;
    }
    return p;
  }

  HFT_ALWAYS_INLINE bool TickOk(Price price) const {
    return tick_size_ == 0 || (price % tick_size_) == 0;
  }

  // --- Open-addressing order table (linear probing, backward-shift delete) ---

  HFT_ALWAYS_INLINE size_t Hash(int64_t order_no) const {
    // Fibonacci hashing of the 64-bit order number.
    uint64_t x = static_cast<uint64_t>(order_no);
    x *= 0x9E3779B97F4A7C15ULL;
    return static_cast<size_t>(x >> 40) & order_mask_;
  }

  OrderRecord* FindOrder(int64_t order_no) {
    size_t idx = Hash(order_no);
    for (size_t probe = 0; probe <= order_mask_; ++probe) {
      OrderRecord& r = orders_[idx];
      if (!r.occupied) {
        return nullptr;
      }
      if (r.order_no == order_no) {
        return &r;
      }
      idx = (idx + 1) & order_mask_;
    }
    return nullptr;
  }

  bool InsertOrder(const OrderRecord& rec) {
    size_t idx = Hash(rec.order_no);
    for (size_t probe = 0; probe <= order_mask_; ++probe) {
      OrderRecord& r = orders_[idx];
      if (!r.occupied) {
        r = rec;
        r.occupied = true;
        return true;
      }
      if (r.order_no == rec.order_no) {  // Duplicate add: overwrite.
        r = rec;
        r.occupied = true;
        return true;
      }
      idx = (idx + 1) & order_mask_;
    }
    return false;  // Table full.
  }

  void EraseOrder(int64_t order_no) {
    size_t idx = Hash(order_no);
    size_t found = order_mask_ + 1;
    for (size_t probe = 0; probe <= order_mask_; ++probe) {
      if (!orders_[idx].occupied) {
        return;
      }
      if (orders_[idx].order_no == order_no) {
        found = idx;
        break;
      }
      idx = (idx + 1) & order_mask_;
    }
    if (found > order_mask_) {
      return;
    }
    // Backward-shift deletion to keep probe sequences intact.
    size_t hole = found;
    size_t next = (hole + 1) & order_mask_;
    while (orders_[next].occupied) {
      size_t home = Hash(orders_[next].order_no);
      // Can orders_[next] move into hole? Yes if home is not strictly between
      // hole and next in circular probe order.
      bool can_move;
      if (hole <= next) {
        can_move = !(hole < home && home <= next);
      } else {
        can_move = !(hole < home || home <= next);
      }
      if (can_move) {
        orders_[hole] = orders_[next];
        hole = next;
      }
      next = (next + 1) & order_mask_;
    }
    orders_[hole].occupied = false;
  }

  // --- Level array maintenance ---

  // Returns index of the level with this price on the given side, or count if
  // not found. `levels`/`count` and the comparator are passed by the caller.
  template <bool IsBid>
  size_t FindLevel(Price price) const {
    const PriceLevel* levels = IsBid ? bids_ : asks_;
    size_t count = IsBid ? bid_count_ : ask_count_;
    // Binary search over the sorted array.
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      Price mp = levels[mid].price;
      bool before;  // True if mid sorts strictly before `price`.
      if (IsBid) {
        before = mp > price;  // Descending.
      } else {
        before = mp < price;  // Ascending.
      }
      if (levels[mid].price == price) {
        return mid;
      }
      if (before) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return count;  // Not found (lo is the insertion point).
  }

  template <bool IsBid>
  size_t InsertionPoint(Price price) const {
    const PriceLevel* levels = IsBid ? bids_ : asks_;
    size_t count = IsBid ? bid_count_ : ask_count_;
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      bool before = IsBid ? (levels[mid].price > price) : (levels[mid].price < price);
      if (before) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  template <bool IsBid>
  void AddQtyToLevel(Price price, Quantity qty) {
    PriceLevel* levels = IsBid ? bids_ : asks_;
    size_t& count = IsBid ? bid_count_ : ask_count_;
    size_t idx = FindLevel<IsBid>(price);
    if (idx < count) {
      levels[idx].add_order(qty);
      return;
    }
    // Insert a new level, keeping the array sorted.
    size_t pos = InsertionPoint<IsBid>(price);
    if (count >= kMaxLevelsPerSide) {
      // Array full: only insert if this level is better than the worst.
      if (pos >= kMaxLevelsPerSide) {
        return;  // New level is worse than everything we keep; drop it.
      }
      // Shift right, discarding the worst (last) level.
      for (size_t i = kMaxLevelsPerSide - 1; i > pos; --i) {
        levels[i] = levels[i - 1];
      }
      levels[pos] = PriceLevel{price, 0, 0};
      levels[pos].add_order(qty);
      return;
    }
    for (size_t i = count; i > pos; --i) {
      levels[i] = levels[i - 1];
    }
    levels[pos] = PriceLevel{price, 0, 0};
    levels[pos].add_order(qty);
    ++count;
  }

  template <bool IsBid>
  void RemoveQtyFromLevel(Price price, Quantity qty, bool drop_order) {
    PriceLevel* levels = IsBid ? bids_ : asks_;
    size_t& count = IsBid ? bid_count_ : ask_count_;
    size_t idx = FindLevel<IsBid>(price);
    if (idx >= count) {
      return;
    }
    levels[idx].remove_order(qty, drop_order);
    if (levels[idx].empty()) {
      for (size_t i = idx; i + 1 < count; ++i) {
        levels[i] = levels[i + 1];
      }
      --count;
    }
  }

  // --- Event handlers ---

  BookResult Seed(const MarketEvent& ev) {
    clear();
    expected_seq_ = ev.seq_no != 0 ? ev.seq_no + 1 : expected_seq_;
    stale_ = false;
    if (ev.bid_qty > 0 && TickOk(ev.bid_price)) {
      bids_[0] = PriceLevel{ev.bid_price, ev.bid_qty, 1};
      bid_count_ = 1;
    }
    if (ev.ask_qty > 0 && TickOk(ev.ask_price)) {
      asks_[0] = PriceLevel{ev.ask_price, ev.ask_qty, 1};
      ask_count_ = 1;
    }
    return BookResult::kSeeded;
  }

  BookResult ApplyAdd(const MarketEvent& ev) {
    if (!TickOk(ev.price)) {
      return BookResult::kTickViolation;
    }
    OrderRecord rec{ev.order_no, ev.price, ev.qty, ev.side, true};
    if (!InsertOrder(rec)) {
      return BookResult::kFull;
    }
    if (ev.side == kBuy) {
      AddQtyToLevel<true>(ev.price, ev.qty);
    } else {
      AddQtyToLevel<false>(ev.price, ev.qty);
    }
    return BookResult::kOk;
  }

  BookResult ApplyModify(const MarketEvent& ev) {
    if (!TickOk(ev.price)) {
      return BookResult::kTickViolation;
    }
    OrderRecord* rec = FindOrder(ev.order_no);
    if (rec == nullptr) {
      return BookResult::kUnknownOrder;
    }
    // Remove old contribution.
    if (rec->side == kBuy) {
      RemoveQtyFromLevel<true>(rec->price, rec->qty, /*drop_order=*/true);
    } else {
      RemoveQtyFromLevel<false>(rec->price, rec->qty, /*drop_order=*/true);
    }
    // Apply new price/qty (NSE re-queues a modified order).
    rec->price = ev.price;
    rec->qty = ev.qty;
    if (rec->side == kBuy) {
      AddQtyToLevel<true>(ev.price, ev.qty);
    } else {
      AddQtyToLevel<false>(ev.price, ev.qty);
    }
    return BookResult::kOk;
  }

  BookResult ApplyCancel(const MarketEvent& ev) {
    OrderRecord* rec = FindOrder(ev.order_no);
    if (rec == nullptr) {
      return BookResult::kUnknownOrder;
    }
    if (rec->side == kBuy) {
      RemoveQtyFromLevel<true>(rec->price, rec->qty, /*drop_order=*/true);
    } else {
      RemoveQtyFromLevel<false>(rec->price, rec->qty, /*drop_order=*/true);
    }
    EraseOrder(ev.order_no);
    return BookResult::kOk;
  }

  BookResult ApplyTrade(const MarketEvent& ev) {
    // A trade reduces both resting orders it matched. MTBT carries the buy
    // resting order in order_no and the sell resting order in order_no2.
    const bool a = ReduceResting(ev.order_no, ev.qty);
    const bool b = ReduceResting(ev.order_no2, ev.qty);
    return (a || b) ? BookResult::kOk : BookResult::kUnknownOrder;
  }

  // Reduce a single resting order by `traded`, removing it when fully consumed.
  // Returns true if the order was found.
  bool ReduceResting(int64_t order_no, Quantity traded) {
    if (order_no == 0) {
      return false;
    }
    OrderRecord* rec = FindOrder(order_no);
    if (rec == nullptr) {
      return false;
    }
    const bool fully = traded >= rec->qty;
    const Quantity reduce = fully ? rec->qty : traded;
    if (rec->side == kBuy) {
      RemoveQtyFromLevel<true>(rec->price, reduce, fully);
    } else {
      RemoveQtyFromLevel<false>(rec->price, reduce, fully);
    }
    if (fully) {
      EraseOrder(order_no);
    } else {
      rec->qty -= reduce;
    }
    return true;
  }

  Token token_;
  Price tick_size_;
  size_t order_mask_;
  std::vector<OrderRecord> orders_;  // Pre-allocated once; never grows.

  PriceLevel bids_[kMaxLevelsPerSide];  // Sorted descending (best first).
  PriceLevel asks_[kMaxLevelsPerSide];  // Sorted ascending (best first).
  size_t bid_count_;
  size_t ask_count_;

  uint64_t expected_seq_;
  bool stale_;
  uint64_t gap_count_;
};

}  // namespace hft

#endif  // HFT_ORDER_BOOK_ORDER_BOOK_HPP_
