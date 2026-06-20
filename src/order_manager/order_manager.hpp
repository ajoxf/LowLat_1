// Order manager: owns the lifecycle of every order we send to NSE and turns
// our internal Order structs into NNF wire messages.
//
// State machine per order:
//   PENDING_NEW -> ACKNOWLEDGED -> PARTIALLY_FILLED -> FILLED
//                              \-> CANCELLED
//                              \-> REJECTED (with NSE error code)
#ifndef HFT_ORDER_MANAGER_ORDER_MANAGER_HPP_
#define HFT_ORDER_MANAGER_ORDER_MANAGER_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "common/compiler.hpp"
#include "common/types.hpp"
#include "order_manager/nnf_encoder.hpp"

namespace hft {

enum class OrderState : uint8_t {
  kIdle = 0,
  kPendingNew,
  kAcknowledged,
  kPartiallyFilled,
  kFilled,
  kCancelled,
  kRejected,
};

struct LiveOrder {
  Order order;
  OrderState state = OrderState::kIdle;
  int64_t exchange_order_no = 0;
  Quantity filled_qty = 0;
  int16_t reject_code = 0;
  bool occupied = false;
};

inline constexpr size_t kOrderTableSize = 4096;  // Power of two.

class OrderManager {
 public:
  OrderManager() : next_clordid_(1), live_count_(0) {
    encoder_.set_alpha_char(' ', ' ');
  }

  NnfEncoder& encoder() { return encoder_; }

  // Assign a fresh, unique client order id.
  ClOrdId next_clordid() { return next_clordid_.fetch_add(1, std::memory_order_relaxed); }

  // Record a new order and encode its NNF NEW ORDER message into buf. If
  // o.client_order_id is 0 a fresh id is assigned. Returns the encoded length,
  // or 0 on duplicate id / table slot conflict (logged by the caller).
  size_t new_order(Order& o, uint8_t* buf, Timestamp now_ist, char open_close = 'O') {
    if (o.client_order_id == 0) {
      o.client_order_id = next_clordid();
    }
    LiveOrder& slot = table_[Index(o.client_order_id)];
    if (slot.occupied && slot.state != OrderState::kFilled &&
        slot.state != OrderState::kCancelled && slot.state != OrderState::kRejected) {
      if (slot.order.client_order_id == o.client_order_id) {
        return 0;  // Duplicate live client order id.
      }
      return 0;  // Slot conflict (table pressure).
    }
    slot.order = o;
    slot.state = OrderState::kPendingNew;
    slot.exchange_order_no = 0;
    slot.filled_qty = 0;
    slot.reject_code = 0;
    slot.occupied = true;
    ++live_count_;
    return encoder_.encode_new_order(buf, o, now_ist, open_close);
  }

  // NSE acknowledged the order: store the exchange order number.
  bool on_ack(ClOrdId clordid, int64_t exchange_order_no) {
    LiveOrder* lo = get(clordid);
    if (lo == nullptr || lo->state != OrderState::kPendingNew) {
      return false;
    }
    lo->exchange_order_no = exchange_order_no;
    lo->order.exchange_order_no = exchange_order_no;
    lo->state = OrderState::kAcknowledged;
    return true;
  }

  // Apply a fill; transitions to PARTIALLY_FILLED or FILLED.
  bool on_fill(const FillReport& fill) {
    LiveOrder* lo = get(fill.client_order_id);
    if (lo == nullptr) {
      return false;
    }
    lo->filled_qty += fill.fill_qty;
    if (lo->filled_qty >= lo->order.qty || fill.remaining_qty == 0) {
      lo->state = OrderState::kFilled;
      Release(lo);
    } else {
      lo->state = OrderState::kPartiallyFilled;
    }
    return true;
  }

  // Cancel acknowledged: remove from the live table.
  bool on_cancel_ack(ClOrdId clordid) {
    LiveOrder* lo = get(clordid);
    if (lo == nullptr) {
      return false;
    }
    lo->state = OrderState::kCancelled;
    Release(lo);
    return true;
  }

  // NSE rejected the order: record the error code, remove from the live table.
  bool on_reject(ClOrdId clordid, int16_t error_code) {
    LiveOrder* lo = get(clordid);
    if (lo == nullptr) {
      return false;
    }
    lo->state = OrderState::kRejected;
    lo->reject_code = error_code;
    Release(lo);
    return true;
  }

  // Encode a cancel for a live, acknowledged order.
  size_t encode_cancel(ClOrdId clordid, uint8_t* buf, Timestamp now_ist) {
    LiveOrder* lo = get(clordid);
    if (lo == nullptr || lo->exchange_order_no == 0) {
      return 0;
    }
    return encoder_.encode_cancel(buf, lo->exchange_order_no, lo->order.token, clordid, now_ist);
  }

  // Returns the slot for clordid whether live or in a terminal state (the
  // clordid is retained until the slot is reused), else nullptr.
  LiveOrder* get(ClOrdId clordid) {
    LiveOrder& slot = table_[Index(clordid)];
    return slot.order.client_order_id == clordid ? &slot : nullptr;
  }

  OrderState state_of(ClOrdId clordid) {
    LiveOrder* lo = get(clordid);
    return lo != nullptr ? lo->state : OrderState::kIdle;
  }

  size_t live_count() const { return live_count_; }

 private:
  HFT_ALWAYS_INLINE static size_t Index(ClOrdId clordid) {
    return static_cast<size_t>(clordid) & (kOrderTableSize - 1);
  }

  void Release(LiveOrder* lo) {
    if (lo->occupied) {
      lo->occupied = false;
      if (live_count_ > 0) {
        --live_count_;
      }
    }
  }

  NnfEncoder encoder_;
  LiveOrder table_[kOrderTableSize];
  std::atomic<ClOrdId> next_clordid_;
  size_t live_count_;
};

}  // namespace hft

#endif  // HFT_ORDER_MANAGER_ORDER_MANAGER_HPP_
